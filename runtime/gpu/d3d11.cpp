// The title's Direct3D 8 calls drawn with Direct3D 11
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend_common.h"
#include "gpu/texturedumper.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr UINT kSwapBuffers = 3;
constexpr int kLateCopies = 3;           // copies in flight per surface locked without a wait
constexpr uint64_t kStaleFrames = 8;     // frames a target may go unused and still be read back
constexpr UINT kRingBytes = 64u << 20;   // vertices and indices between wraps
constexpr size_t kStateObjectsMax = 4000;  // Direct3D 11 keeps at most 4096 of each kind
constexpr UINT kPassConstantBytes = 256;
constexpr uint32_t kQueries = 8192;  // occlusion queries in flight, several frames of counted draws
constexpr uint32_t kNoQuery = ~0u;
constexpr float kLodBiasMin = -16.0f;  // D3D11_MIP_LOD_BIAS_MIN and MAX
constexpr float kLodBiasMax = 15.99f;

// colour, depth and stencil over part of a target, where Direct3D 11 clears whole views only
const char* kRectClearHlsl = R"(
cbuffer C : register(b1) { float4 colour; float4 depth; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), depth.x, 1);
}
float4 ps() : SV_Target { return colour; }
)";

D3D11_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D11_BLEND_ZERO;
        case 1: return D3D11_BLEND_ONE;
        case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D11_BLEND_SRC_ALPHA;
        case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D11_BLEND_DEST_ALPHA;
        case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
        default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D11_BLEND_OP_MIN;
        case 0x8008: return D3D11_BLEND_OP_MAX;
        default: return D3D11_BLEND_OP_ADD;
    }
}

D3D11_COMPARISON_FUNC Cmp(uint32_t v) { return D3D11_COMPARISON_FUNC(CmpFunc(v)); }

// a D3DCMP test on reversed depth: less and greater trade places
D3D11_COMPARISON_FUNC ReversedCmp(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return D3D11_COMPARISON_FUNC(mirror[CmpFunc(v) - 1] + 1);
}

D3D11_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D11_STENCIL_OP_ZERO;
        case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D11_STENCIL_OP_INVERT;
        case 0x8507: return D3D11_STENCIL_OP_INCR;
        case 0x8508: return D3D11_STENCIL_OP_DECR;
        default: return D3D11_STENCIL_OP_KEEP;
    }
}

D3D11_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 3: case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
        default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

// one level, one sample, read by shaders; callers add their bind flags
D3D11_TEXTURE2D_DESC Texture2DDesc(DXGI_FORMAT fmt, UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return td;
}

// bytes of a 4x4 block for the compressed formats, else of a texel
UINT FormatBytes(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_BC1_UNORM) return 8;
    if (f == DXGI_FORMAT_BC2_UNORM || f == DXGI_FORMAT_BC3_UNORM) return 16;
    return 4;
}

bool Compressed(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_BC1_UNORM || f == DXGI_FORMAT_BC2_UNORM || f == DXGI_FORMAT_BC3_UNORM;
}

struct Target : TargetBase {
    ComPtr<ID3D11Texture2D> res;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    // a copy read by draws that sample the target they render to
    ComPtr<ID3D11Texture2D> fb;
    ComPtr<ID3D11ShaderResourceView> fb_srv;
};

struct Tex {
    ComPtr<ID3D11Resource> res;
    ComPtr<ID3D11ShaderResourceView> srv;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint32_t w = 0;  // as created, BC sizes rounded up to whole blocks
    uint32_t h = 0;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    const Tex* swap = nullptr;  // the texture pack's image drawn in its place
    uint64_t pack_key = 0;      // its pack image is still loading
    uint32_t pack_gen = 0;      // texturedumper::Generation the key was taken at, 0 after new bytes
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures : StageInfo {
    ID3D11ShaderResourceView* srv[4];
    uint32_t levels[4];  // mip levels of the view
};

// Shrink's settings past the source and the size; into: a copy from an earlier call to draw over again
struct ShrinkOptions {
    uint32_t taps = 1;
    ID3D11Texture2D* into = nullptr;
    float mul = 1;
};

// a full screen pass's shaders
struct Pass {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
};

// a draw's shaders, the geometry shader that draws points, and the input layout, null when one failed
struct Program {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
};

// what of a PsoKey the blend, depth stencil and rasterizer states are made from
struct StateKey {
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc, sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t fill, cull, ccw, points;
    int32_t zbias;
    float zslope;
};

StateKey MakeStateKey(const PsoKey& k) {
    return {k.blend,  k.src,   k.dst,    k.op,    k.wmask, k.zen,  k.zwrite, k.zfunc,
            k.sten,   k.sfunc, k.smask,  k.swmask, k.sfail, k.szfail, k.spass, k.fill,
            k.cull,   k.ccw,   k.topo == uint32_t(Topology::Points), k.zbias, k.zslope};
}

struct States {
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
};

// a draw's four samplers
struct Samplers {
    ComPtr<ID3D11SamplerState> s[4];
};

// a resource and the view shaders read it through
struct Readable {
    ID3D11Texture2D* res;
    ID3D11ShaderResourceView* srv;
};

class D3D11Backend final : public Backend {
public:
    ~D3D11Backend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points,
              float src_kx, float src_ky) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override { ctx_->Flush(); }
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_}); }

private:
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateBackBufferView();
    void CreateBuffers();
    void CreatePassStates();

    // passes over whole targets
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h);
    void WriteConstants(ID3D11Buffer* b, const void* p, size_t n);
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    const Pass* EnsurePass(uint64_t key, const char* hlsl, const char* vs_entry, const char* ps_entry);
    void BindPass(const Pass& pass, const void* constants, size_t bytes);
    void Blit(ID3D11ShaderResourceView* src, const float uvrect[4], const BlitOptions& o = {});

    // targets
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    bool CreateTargetResource(Target& t);
    bool CreateTargetViews(Target& t);
    void UpdateTargetUsed(uint32_t data);
    bool TargetStale(uint32_t data) const;
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D11ShaderResourceView> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h);
    ID3D11ShaderResourceView* Feedback(Target& t);
    float TargetScale() const;  // host pixels per console pixel of the bound target

    // Draw, in call order, each callee right after its caller
    bool DepthCopy(const State& s);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D11ShaderResourceView* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D11ShaderResourceView* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                                DrawTextures& stages);
    ID3D11ShaderResourceView* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours);
    void LatchPackImage(Tex& tx);
    void CreatePackTexture(Tex& r, const texturedumper::Image& img);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    const Program* EnsureProgram(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    const States& EnsureStates(const PsoKey& k);
    void ApplyStates(const State& s, const PsoKey& k);
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    void SetViewportScissor(const State& s, uint32_t gw);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void DrawGeometry(const Assembled& a);
    UINT Stream(const void* p, UINT size, UINT align, ComPtr<ID3D11Buffer>& buf);

    // visibility tests
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();

    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<RECT>& rs);
    void ClearRects(Target& t, const ClearArgs& c, const float col[4], uint8_t mask, const std::vector<RECT>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
        float kx = 1, ky = 1;  // Copy's src_kx, src_ky
    };
    void CopyByDraw(const Target& src_t, Target& dst_t, const CopyList& l);
    void CopyByRegion(const Target& src_t, Target& dst_t, const CopyList& l);

    // Present, in call order, each callee right after its caller
    void FinishCapture(const Surface& front);
    void ResizeSwap();
    void UploadGammaRamp();
    void DrawFrame(Target* t);
    void BlitFrame(Target& t);
    void RenderUi();
    void PollLate();
    void ReleaseOldTextures();
    void CountFps();
    void ApplySettings();

    // reading targets back
    struct CopySource {
        ID3D11Texture2D* res;
        ComPtr<ID3D11Texture2D> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    ComPtr<ID3D11Texture2D> Shrink(ID3D11ShaderResourceView* src, uint32_t w, uint32_t h, const ShrinkOptions& o);
    bool ReadBack(ID3D11Texture2D* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void ReadRows(ID3D11Texture2D* staging, uint32_t w, uint32_t h, uint8_t* out, uint32_t pitch);
    struct Late;
    void ResetLate(Late& l, const Target& t);
    void RecordLateCopy(Late& l, ID3D11Texture2D* src);

    // F3 capture: a view CapSave shrinks to w x h and writes, colour times mul
    void CapSave(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, float mul, const char* name);
    void CapSave(Target& t, const char* name);
    void CapStep(const Surface& rt);

    ComPtr<IDXGIFactory2> factory_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> bb_rtv_;
    bool tearing_ = false;
    bool ready_ = false;  // ImGui's renderer is set up
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;

    // vertices and indices in one ring, constants per draw
    ComPtr<ID3D11Buffer> ring_;
    UINT ring_pos_ = kRingBytes;  // the first write discards
    ComPtr<ID3D11Buffer> vsc_cb_;
    ComPtr<ID3D11Buffer> dc_cb_;
    ComPtr<ID3D11Buffer> pass_cb_;
    // what the constant buffers hold, so a draw with the same constants writes none
    decltype(State::vsc) last_vsc_{};
    DrawConstants last_dc_{};
    bool constants_held_ = false;

    // full screen passes: no depth, no culling, the scissor set with the viewport
    ComPtr<ID3D11RasterizerState> pass_raster_;
    ComPtr<ID3D11DepthStencilState> no_depth_;
    ComPtr<ID3D11DepthStencilState> depth_copy_;   // depth always written
    ComPtr<ID3D11DepthStencilState> clear_depth_[4];  // by kClearZ | kClearStencil
    ComPtr<ID3D11BlendState> over_blend_;
    ComPtr<ID3D11BlendState> channel_blend_[16];  // by colour write mask
    ComPtr<ID3D11SamplerState> blit_samplers_[4];  // linear at slot 0, point at 1 to 3
    std::unordered_map<uint64_t, Pass> passes_;

    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D11VertexShader>> vs_;  // by source hash
    std::unordered_map<uint64_t, ComPtr<ID3D11PixelShader>> ps_;
    ComPtr<ID3D11GeometryShader> point_gs_[2];  // PointHlsl, SpriteHlsl
    std::unordered_map<uint64_t, Program> programs_;  // by shaders, vertex layout and points
    std::unordered_map<uint64_t, States> states_;     // by StateKey
    std::unordered_map<uint64_t, Samplers> samplers_;

    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::array<UsedTarget, 4> recent_used_{};
    uint32_t recent_used_next_ = 0;
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, Tex> pack_textures_;  // texture pack images by key, kept for the run
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    float scale_ = 1.0f;

    ComPtr<ID3D11Texture2D> lut_;
    ComPtr<ID3D11ShaderResourceView> lut_srv_;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;

    // surfaces locked without a wait: copies in flight, polled at present; the newest finished one waits in px for
    // the title's thread to take
    struct Late {
        ComPtr<ID3D11Texture2D> rb[kLateCopies];
        ComPtr<ID3D11Query> done[kLateCopies];
        bool pending[kLateCopies] = {};
        uint64_t frame[kLateCopies] = {};
        ComPtr<ID3D11Texture2D> down;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t next = 0;
        std::vector<uint8_t> px;  // under late_lock_, rows at GuestPitch
        uint64_t px_frame = 0;
        uint64_t px_seq = 0;
        uint64_t delivered = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                    // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};  // presents_ for DeliverLate

    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        uint32_t index;
        float samples_per_pixel;  // host samples one console pixel covers
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
    };
    std::vector<ComPtr<ID3D11Query>> queries_ = std::vector<ComPtr<ID3D11Query>>(kQueries);  // made on first use
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;  // since the last ClearReport
    std::deque<Report> reports_;

    CaptureLog cap_;
    struct CapTex {
        ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;

    Assembled asm_;  // kept between draws for its buffers
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D11Backend::Init(uint32_t width, uint32_t height) {
    g_reversed_depth = true;
    guest_w_ = width;
    guest_h_ = height;
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    CreateBackBufferView();
    CreateBuffers();
    CreatePassStates();
    ready_ = ImGui_ImplDX11_Init(dev_.Get(), ctx_.Get());
    scale_ = app::RenderScale(guest_h_);
    texturedumper::Update();
    LOGI("d3d11: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

// another renderer may take the window next: the swap chain goes before the context lets go of it
D3D11Backend::~D3D11Backend() {
    if (ready_) {
        std::lock_guard lock(app::UiLock());
        ImGui_ImplDX11_Shutdown();
    }
    bb_rtv_.Reset();
    swap_.Reset();
    if (!ctx_) return;
    ctx_->ClearState();
    ctx_->Flush();
}

bool D3D11Backend::CreateDevice() {
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    // dedicated GPU first on hybrid laptops
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f6)))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), type, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev_, nullptr,
                                   &ctx_);
    if (FAILED(hr) && adapter) {
        adapter = nullptr;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev_,
                               nullptr, &ctx_);
    }
    if (FAILED(hr)) {
        LOGE("d3d11: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D11 adapter: %ls", adapter ? ad.Description : L"default");
    // the swap chain comes from the factory that made the device's adapter
    ComPtr<IDXGIDevice1> dxgi;
    ComPtr<IDXGIAdapter> used;
    if (FAILED(dev_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&used)) || FAILED(used->GetParent(IID_PPV_ARGS(&factory_)))) {
        LOGE("d3d11: no DXGI factory");
        return false;
    }
    // the title records the next frame while this one draws, as Direct3D 12 waits on the frame before
    dxgi->SetMaximumFrameLatency(1);
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D11Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = kBgra;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSwapBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(factory_->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &swap_))) {
        LOGE("d3d11: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

// a flip model swap chain under Direct3D 11 draws to buffer 0, which is always the next one shown
void D3D11Backend::CreateBackBufferView() {
    ComPtr<ID3D11Texture2D> bb;
    swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
    dev_->CreateRenderTargetView(bb.Get(), nullptr, &bb_rtv_);
}

void D3D11Backend::CreateBuffers() {
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = kRingBytes;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
    dev_->CreateBuffer(&bd, nullptr, &ring_);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = UINT(sizeof(State::vsc));
    dev_->CreateBuffer(&bd, nullptr, &vsc_cb_);
    bd.ByteWidth = UINT((sizeof(DrawConstants) + 15) & ~size_t(15));
    dev_->CreateBuffer(&bd, nullptr, &dc_cb_);
    bd.ByteWidth = kPassConstantBytes;
    dev_->CreateBuffer(&bd, nullptr, &pass_cb_);
    // gamma ramp as a 256 x 1 lookup
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, 256, 1);
    dev_->CreateTexture2D(&td, nullptr, &lut_);
    dev_->CreateShaderResourceView(lut_.Get(), nullptr, &lut_srv_);
}

void D3D11Backend::CreatePassStates() {
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    dev_->CreateRasterizerState(&rd, &pass_raster_);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dev_->CreateDepthStencilState(&dd, &no_depth_);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dev_->CreateDepthStencilState(&dd, &depth_copy_);
    // premultiplied colour, the target keeps its alpha
    D3D11_BLEND_DESC bd{};
    auto& b = bd.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D11_BLEND_ONE;
    b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.SrcBlendAlpha = D3D11_BLEND_ZERO;
    b.DestBlendAlpha = D3D11_BLEND_ONE;
    b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev_->CreateBlendState(&bd, &over_blend_);
    // D3DCLEAR_TARGET_R/G/B/A are bits 4 to 7, Direct3D's write mask has the same order
    for (uint8_t mask = 0; mask < 16; mask++) {
        D3D11_BLEND_DESC cd{};
        cd.RenderTarget[0].RenderTargetWriteMask = mask;
        dev_->CreateBlendState(&cd, &channel_blend_[mask]);
    }
    for (int i = 0; i < 4; i++) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = i == 0 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&sd, &blit_samplers_[i]);
    }
    for (uint32_t flags = 0; flags < 4; flags++) {
        D3D11_DEPTH_STENCIL_DESC cd{};
        cd.DepthEnable = (flags & kClearZ) != 0;
        cd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        cd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        cd.StencilEnable = (flags & kClearStencil) != 0;
        cd.StencilReadMask = cd.StencilWriteMask = 0xFF;
        D3D11_DEPTH_STENCILOP_DESC so{D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
                                      D3D11_COMPARISON_ALWAYS};
        cd.FrontFace = cd.BackFace = so;
        dev_->CreateDepthStencilState(&cd, &clear_depth_[flags]);
    }
}

void D3D11Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D11_RECT scissor{0, 0, LONG(w), LONG(h)};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetScissorRects(1, &scissor);
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D11Backend::SetBlitTarget(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h) {
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

void D3D11Backend::WriteConstants(ID3D11Buffer* b, const void* p, size_t n) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, p, n);
    ctx_->Unmap(b, 0);
}

ID3DBlob* D3D11Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(src.data(), src.size(), entry_seed);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("d3d11: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    blobs_[hash] = code;
    return code.Get();
}

// the pass's shaders, kept under key; null when they can't be made
const Pass* D3D11Backend::EnsurePass(uint64_t key, const char* hlsl, const char* vs_entry, const char* ps_entry) {
    auto it = passes_.find(key);
    if (it != passes_.end()) return it->second.vs ? &it->second : nullptr;
    Pass& p = passes_[key];
    uint64_t h;
    ID3DBlob* vs = Compile(hlsl, vs_entry, "vs_5_0", h);
    ID3DBlob* ps = Compile(hlsl, ps_entry, "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &p.vs);
    dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &p.ps);
    if (!p.vs || !p.ps) {
        p = {};
        return nullptr;
    }
    return &p;
}

// the full screen triangle's shaders and states: constants at b0 and b1, the blit samplers, no depth, no blend;
// callers bind the views and the target
void D3D11Backend::BindPass(const Pass& pass, const void* constants, size_t bytes) {
    WriteConstants(pass_cb_.Get(), constants, bytes);
    constants_held_ = false;
    ID3D11Buffer* cbs[2] = {pass_cb_.Get(), pass_cb_.Get()};
    ctx_->VSSetConstantBuffers(0, 2, cbs);
    ctx_->PSSetConstantBuffers(0, 2, cbs);
    ID3D11SamplerState* smp[4];
    for (int i = 0; i < 4; i++) smp[i] = blit_samplers_[i].Get();
    ctx_->PSSetSamplers(0, 4, smp);
    ctx_->VSSetShader(pass.vs.Get(), nullptr, 0);
    ctx_->GSSetShader(nullptr, nullptr, 0);
    ctx_->PSSetShader(pass.ps.Get(), nullptr, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->RSSetState(pass_raster_.Get());
    ctx_->OMSetDepthStencilState(no_depth_.Get(), 0);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

constexpr uint64_t kBlitPassKey = 0xB117;

void D3D11Backend::Blit(ID3D11ShaderResourceView* src, const float uvrect[4], const BlitOptions& o) {
    const Pass* pass = EnsurePass(kBlitPassKey, kBlitHlsl, "vs", "psmain");
    if (!pass) return;
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                  o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    ID3D11ShaderResourceView* views[2] = {src, lut_srv_.Get()};
    ctx_->PSSetShaderResources(0, 2, views);
    BindPass(*pass, c, sizeof(c));
    if (o.over) ctx_->OMSetBlendState(over_blend_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->Draw(3, 0);
}

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D11Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// the surface's target at its size, null when there is none
Target* D3D11Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(SizedTargetKey(data, depth, gw, gh));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    UpdateTargetUsed(data);
    return &it->second;
}

// the surface's target, made again at its size when that changed
Target* D3D11Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s, gw, gh);
    uint64_t key = SizedTargetKey(data, depth, gw, gh);
    UpdateTargetUsed(data);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) {
        if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
        targets_.erase(it);
    }
    Target t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    if (!CreateTargetResource(t) || !CreateTargetViews(t)) return nullptr;
    g_perf.targets++;
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    auto& ref = targets_[key] = std::move(t);
    return &ref;
}

// the host texture of a new target, at render scale
bool D3D11Backend::CreateTargetResource(Target& t) {
    t.w = std::max(1u, uint32_t(std::lround(t.gw * scale_)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * scale_)));
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(t.depth ? kDepthTypeless : t.fmt, t.w, t.h);
    td.BindFlags |= t.depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &t.res));
}

// the target's views, cleared as a new Direct3D 12 target starts
bool D3D11Backend::CreateTargetViews(Target& t) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (t.depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = kDepthFormat;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(dev_->CreateDepthStencilView(t.res.Get(), &dv, &t.dsv))) return false;
        sv.Format = kDepthRead;
        ctx_->ClearDepthStencilView(t.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, kDepthFar, 0);
    } else {
        if (FAILED(dev_->CreateRenderTargetView(t.res.Get(), nullptr, &t.rtv))) return false;
        sv.Format = t.fmt;
        float black[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(t.rtv.Get(), black);
    }
    return SUCCEEDED(dev_->CreateShaderResourceView(t.res.Get(), &sv, &t.srv));
}

void D3D11Backend::UpdateTargetUsed(uint32_t data) {
    for (const UsedTarget& u : recent_used_) {
        if (u.data == data && u.frame == presents_) return;
    }
    target_used_[data] = presents_;
    recent_used_[recent_used_next_++ % recent_used_.size()] = {data, presents_};
}

// long unused: the title freed it and other data is at this address now
bool D3D11Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return presents_ - used > kStaleFrames;
}

// BGRA pixels into a new w x h texture
ComPtr<ID3D11ShaderResourceView> D3D11Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch,
                                                                  uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, w, h);
    td.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA init{px.data(), pitch, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(dev_->CreateTexture2D(&td, &init, &tex))) dev_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

// CPU pixels of a color surface into its host target, scaled
void D3D11Backend::UploadTarget(Target& t, const Surface& s) {
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d11: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D11ShaderResourceView> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    if (!tex) return;
    SetBlitTarget(t.rtv.Get(), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(tex.Get(), uv, {.alpha = true, .mul = 1 / t.range, .rtv = t.fmt, .over = over});
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// a copy of the target to read while drawing to it
ID3D11ShaderResourceView* D3D11Backend::Feedback(Target& t) {
    if (!t.fb) {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(t.fmt, t.w, t.h);
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t.fb))) return nullptr;
        dev_->CreateShaderResourceView(t.fb.Get(), nullptr, &t.fb_srv);
    }
    ctx_->CopyResource(t.fb.Get(), t.res.Get());
    return t.fb_srv.Get();
}

float D3D11Backend::TargetScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

constexpr uint64_t kDepthCopyKey = ~0ull;  // DepthCopy's shaders in passes_

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D11Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    const Pass* pass = EnsurePass(kDepthCopyKey, kDepthHlsl, "vs", "psmain");
    if (!pass) return false;
    Target& dst = di->second;
    Target& src = si->second;
    if (cap_.file) fprintf(cap_.file, "%04u depth copy %08X to %08X\n", cap_.i++, src.data, dst.data);
    ctx_->OMSetRenderTargets(0, nullptr, dst.dsv.Get());
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(dst.w, dst.h);
    ID3D11ShaderResourceView* view = src.srv.Get();
    ctx_->PSSetShaderResources(0, 1, &view);
    float none[4] = {};
    BindPass(*pass, none, sizeof(none));
    ctx_->OMSetDepthStencilState(depth_copy_.Get(), 0);
    ctx_->Draw(3, 0);
    dst.drawn = true;
    return true;
}

bool D3D11Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        rt->drawn = true;
    }
    if (ds) ds->drawn = true;
    if (rt != bound_rt_ || ds != bound_ds_) {
        ID3D11RenderTargetView* rtv = rt ? rt->rtv.Get() : nullptr;
        ctx_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rtv : nullptr, ds ? ds->dsv.Get() : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    return true;
}

// sets sh.vh and sh.vs_code, compiling the shader on a cache miss
void D3D11Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second].Get();
        sh.vh = vit->second;
        return;
    }
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

bool D3D11Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    {
        PerfScope perf_bind{g_perf.bind};
        for (int i = 0; i < 4; i++) stages.srv[i] = BindTexture(s, i, stages);
    }
    // textures may have drawn copies that unbound the targets
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    return true;
}

ID3D11ShaderResourceView* D3D11Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

ID3D11ShaderResourceView* D3D11Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                                          DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    info.flags |= kTexTarget;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    return t.srv.Get();
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D11ShaderResourceView* D3D11Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d,
                                                         const FormatInfo& fi, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, p, colours);
            tx.pack_gen = 0;
        }
    }
    if (tx.pack_gen != texturedumper::Generation()) LatchPackKey(tx, d, fi, colours);
    if (tx.pack_key) LatchPackImage(tx);
    const Tex& drawn = tx.swap ? *tx.swap : tx;
    stages.levels[stage] = drawn.levels;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    return drawn.srv.Get();
}

// false when the texture could not be made, its entry dropped
bool D3D11Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    tx.w = d.width;
    tx.h = d.height;
    tx.levels = d.levels;
    if (fi.compressed && ((tx.w & 3) || (tx.h & 3))) {
        tx.w = (tx.w + 3) & ~3u;
        tx.h = (tx.h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = tx.fmt;
    HRESULT hr;
    if (d.depth > 1) {
        D3D11_TEXTURE3D_DESC td{tx.w, tx.h, d.depth, tx.levels, tx.fmt, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ComPtr<ID3D11Texture3D> t3;
        hr = dev_->CreateTexture3D(&td, nullptr, &t3);
        tx.res = t3;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(tx.fmt, tx.w, tx.h);
        td.MipLevels = tx.levels;
        td.ArraySize = d.cube ? 6 : 1;
        td.MiscFlags = d.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        ComPtr<ID3D11Texture2D> t2;
        hr = dev_->CreateTexture2D(&td, nullptr, &t2);
        tx.res = t2;
        sv.ViewDimension = d.cube ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = tx.levels;
    }
    if (FAILED(hr) || FAILED(dev_->CreateShaderResourceView(tx.res.Get(), &sv, &tx.srv))) {
        textures_.erase(key);
        return false;
    }
    return true;
}

// one level as UpdateSubresource reads it: rows of row_size bytes, pitch apart, slices of rows each; px is used as
// it is when it holds them all, else copied into scratch with the missing rows zero
struct LevelRows {
    const uint8_t* p;
    UINT pitch;
    UINT slice;
};

LevelRows PadLevel(const std::vector<uint8_t>& px, uint32_t pitch, UINT row_size, UINT rows, UINT slices,
                   std::vector<uint8_t>& scratch) {
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    if (pitch >= row_size && src_rows >= rows) return {px.data(), pitch, UINT(src_rows * pitch)};
    scratch.assign(size_t(row_size) * rows * slices, 0);
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows && y < src_rows; y++) {
            uint8_t* dst = scratch.data() + (size_t(z) * rows + y) * row_size;
            std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
        }
    }
    return {scratch.data(), row_size, row_size * rows};
}

void D3D11Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    UINT w = std::max(1u, tx.w >> l);
    UINT h = std::max(1u, tx.h >> l);
    UINT slices = std::max(1u, tx.d.depth >> l);
    bool bc = Compressed(tx.fmt);
    UINT row_size = (bc ? (w + 3) / 4 : w) * FormatBytes(tx.fmt);
    UINT rows = bc ? (h + 3) / 4 : h;
    std::vector<uint8_t> scratch;
    LevelRows r = PadLevel(px, pitch, row_size, rows, slices, scratch);
    ctx_->UpdateSubresource(tx.res.Get(), D3D11CalcSubresource(l, f, tx.levels), nullptr, r.p, r.pitch, r.slice);
}

void D3D11Backend::UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours) {
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
}

// after new bytes or a texture setting change: the name the pack and the dumps know the bytes by
void D3D11Backend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    uint64_t key = texturedumper::Key(d, fi, p, colours);
    texturedumper::Dump(key, d, p, colours);
    // dumping alone keys the bytes too: the pack image is not drawn then
    tx.pack_key = texturedumper::Replacing() ? key : 0;
}

// the pack image for tx's bytes once it has loaded; textures with the same bytes share its GPU copy
void D3D11Backend::LatchPackImage(Tex& tx) {
    auto it = pack_textures_.find(tx.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        texturedumper::Lookup got = texturedumper::Take(tx.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tx.pack_key = 0;
            return;
        }
        it = pack_textures_.emplace(tx.pack_key, Tex{}).first;
        CreatePackTexture(it->second, img);
    }
    if (it->second.srv) tx.swap = &it->second;
    tx.pack_key = 0;
}

void D3D11Backend::CreatePackTexture(Tex& r, const texturedumper::Image& img) {
    r.fmt = img.format;
    r.levels = uint32_t(img.levels.size());
    r.w = img.width;
    r.h = img.height;
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(r.fmt, img.width, img.height);
    td.MipLevels = r.levels;
    ComPtr<ID3D11Texture2D> t2;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t2))) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    r.res = t2;
    for (uint32_t l = 0; l < r.levels; l++) UploadTextureLevel(r, 0, l, img.levels[l].px, img.levels[l].pitch);
    dev_->CreateShaderResourceView(r.res.Get(), nullptr, &r.srv);
}

// sets sh.ph and sh.ps_code, compiling the shader on a cache miss
void D3D11Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second].Get();
        sh.ph = pit->second;
        return;
    }
    sh.ps_code = Compile(PixelHlsl(pk), "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void D3D11Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, bound_ds_, k);
    KeyRasterizer(s, a, k);
}

// the program for the key's shaders, vertex layout and points, made once
const Program* D3D11Backend::EnsureProgram(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    bool points = k.topo == uint32_t(Topology::Points);
    uint64_t shaders[3] = {k.vs, k.ps, points ? 1 + uint64_t(k.sprite) : 0};
    uint64_t key = Hash(k.elements, sizeof(k.elements), Hash(shaders, sizeof(shaders), k.regs));
    auto it = programs_.find(key);
    if (it != programs_.end()) return &it->second;
    g_perf.psos++;
    Program& p = programs_[key];
    ComPtr<ID3D11VertexShader>& v = vs_[k.vs];
    if (!v) dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &v);
    ComPtr<ID3D11PixelShader>& f = ps_[k.ps];
    if (!f) dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &f);
    // a point is drawn as its whole console pixel, or a sprite as its square, by a geometry shader
    if (points) {
        ComPtr<ID3D11GeometryShader>& gs = point_gs_[k.sprite];
        uint64_t h;
        ID3DBlob* code = gs ? nullptr : Compile(k.sprite ? SpriteHlsl() : PointHlsl(), "main", "gs_5_0", h);
        if (code) dev_->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &gs);
        p.gs = gs;
    }
    D3D11_INPUT_ELEMENT_DESC el[16];
    UINT n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16, D3D11_INPUT_PER_VERTEX_DATA, 0};
    }
    if (n && FAILED(dev_->CreateInputLayout(el, n, vs->GetBufferPointer(), vs->GetBufferSize(), &p.layout))) {
        LOGE("d3d11: input layout creation failed");
        return &p;
    }
    p.vs = v;
    p.ps = f;
    return &p;
}

// blend, depth stencil and rasterizer states as the key has them, dropped together when Direct3D 11's limit nears
const States& D3D11Backend::EnsureStates(const PsoKey& k) {
    StateKey sk = MakeStateKey(k);
    uint64_t key = Hash(&sk, sizeof(sk));
    auto it = states_.find(key);
    if (it != states_.end()) return it->second;
    if (states_.size() >= kStateObjectsMax) states_.clear();
    States& st = states_[key];
    D3D11_BLEND_DESC bd{};
    auto& b = bd.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    dev_->CreateBlendState(&bd, &st.blend);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = k.zen;
    dd.DepthWriteMask = k.zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = ReversedCmp(k.zfunc);
    dd.StencilEnable = k.sten;
    dd.StencilReadMask = UINT8(k.smask);
    dd.StencilWriteMask = UINT8(k.swmask);
    D3D11_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    dd.FrontFace = dd.BackFace = so;
    dev_->CreateDepthStencilState(&dd, &st.depth);
    // points are drawn by the geometry shader solid, front facing and never culled
    bool points = k.topo == uint32_t(Topology::Points);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = k.fill == kFillWireframe && !points ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.CullMode = k.cull && !points ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    rd.FrontCounterClockwise = k.ccw && !points;
    rd.DepthBias = k.zbias;
    rd.SlopeScaledDepthBias = k.zslope;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    dev_->CreateRasterizerState(&rd, &st.raster);
    return st;
}

// the key's states with the blend factor and stencil reference
void D3D11Backend::ApplyStates(const State& s, const PsoKey& k) {
    const States& st = EnsureStates(k);
    float factor[4];
    Color(SimpleState(s, kMBlendColor), factor);
    ctx_->OMSetBlendState(st.blend.Get(), factor, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(st.depth.Get(), SimpleState(s, kMStencilRef) & 0xFF);
    ctx_->RSSetState(st.raster.Get());
}

void D3D11Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestExtent size, DrawConstants& c) {
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = TargetScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    FillDepthConstants(s, sh, k, c);
    bool reads_target = stages.rt[0] || stages.rt[1] || stages.rt[2] || stages.rt[3];
    float rt_shift = reads_target ? RenderTargetShift(s, sh, TargetScale()) : 0.0f;
    FillTextureConstants(s, stages, rt_shift, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    if (!constants_held_ || std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc))) {
        WriteConstants(vsc_cb_.Get(), s.vsc.data(), sizeof(s.vsc));
        std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    }
    if (!constants_held_ || std::memcmp(&last_dc_, &c, sizeof(c))) {
        WriteConstants(dc_cb_.Get(), &c, sizeof(c));
        last_dc_ = c;
    }
    if (!constants_held_) {
        ID3D11Buffer* cbs[2] = {vsc_cb_.Get(), dc_cb_.Get()};
        ctx_->VSSetConstantBuffers(0, 2, cbs);
        ctx_->GSSetConstantBuffers(0, 2, cbs);
        ctx_->PSSetConstantBuffers(0, 2, cbs);
        constants_held_ = true;
    }
}

D3D11_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, bool fixed,
                                uint32_t aniso) {
    D3D11_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D11_FILTER_TYPE fmin = lin(min) ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE fmag = lin(mag) ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE fmip = mip == kTexfLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    sd.Filter = D3D11_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp, unless the fixed
    // function moves this stage's out of that range with a texture matrix or generates them
    if (fixed && ((t[kTssTexCoordIndex] >> 16 & 7) || (t[kTssTextureTransformFlags] & 7))) clamp = false;
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.AddressW = Address(t[kTssAddressW]);
    // the title can ask past the range the hardware takes
    sd.MipLODBias = std::clamp(Bits(t[kTssMipLodBias]), kLodBiasMin, kLodBiasMax);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D11_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, Direct3D tests r OP texel; both reversed (g_reversed_depth) keep the OP
        sd.Filter = lin(min) || lin(mag) ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = Cmp(s.rs[kRsShadowFunc]);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

// a stage without a texture reads a null view, which gives 0 as Direct3D 12's null views do
void D3D11Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    ctx_->PSSetShaderResources(0, 4, stages.srv);
    bool fixed = !a.pretransformed && !((s.vs_handle & 1) && s.vs && !s.vs->program.empty());
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    uint32_t aniso = app::Get().aniso;
    D3D11_SAMPLER_DESC sd[4];
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, fixed, aniso);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it == samplers_.end()) {
        if (samplers_.size() >= kStateObjectsMax / 4) samplers_.clear();
        it = samplers_.emplace(key, Samplers{}).first;
        for (int i = 0; i < 4; i++) dev_->CreateSamplerState(&sd[i], &it->second.s[i]);
    }
    ID3D11SamplerState* smp[4];
    for (int i = 0; i < 4; i++) smp[i] = it->second.s[i].Get();
    ctx_->PSSetSamplers(0, 4, smp);
}

void D3D11Backend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D11_VIEWPORT vp{0, 0, W, H, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    D3D11_RECT rect = ViewportScissorRect(s, gw, W, H);
    ctx_->RSSetScissorRects(1, &rect);
}

// size bytes of p into the ring at the returned offset, or into a buffer of their own when they don't fit it
UINT D3D11Backend::Stream(const void* p, UINT size, UINT align, ComPtr<ID3D11Buffer>& buf) {
    g_perf.upload += size;
    if (size > kRingBytes) {
        g_perf.overflow++;
        D3D11_BUFFER_DESC bd{size, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA init{p, 0, 0};
        dev_->CreateBuffer(&bd, &init, &buf);
        return 0;
    }
    UINT at = (ring_pos_ + align - 1) & ~(align - 1);
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (at + size > kRingBytes) {
        at = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(ring_.Get(), 0, mode, 0, &m))) return 0;
    std::memcpy(static_cast<uint8_t*>(m.pData) + at, p, size);
    ctx_->Unmap(ring_.Get(), 0);
    ring_pos_ = at + size;
    buf = ring_;
    return at;
}

void D3D11Backend::DrawGeometry(const Assembled& a) {
    ComPtr<ID3D11Buffer> bufs[16];
    ID3D11Buffer* raw[16];
    UINT strides[16];
    UINT offsets[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        offsets[i] = Stream(b.src, b.bytes, 16, bufs[i]);
        raw[i] = bufs[i].Get();
        strides[i] = b.stride;
    }
    ctx_->IASetVertexBuffers(0, a.buffer_count, raw, strides, offsets);
    ctx_->IASetPrimitiveTopology(PrimitiveTopology(a));
    if (!a.index_count) {
        ctx_->Draw(a.draw_count, 0);
        return;
    }
    ComPtr<ID3D11Buffer> ib;
    UINT ibytes = a.index_count * (a.index16 ? 2 : 4);
    UINT at = Stream(a.index_src, ibytes, 4, ib);
    ctx_->IASetIndexBuffer(ib.Get(), a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, at);
    ctx_->DrawIndexed(a.index_count, 0, a.base_vertex);
}

void D3D11Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (SpriteOnStage3(s, prim)) {
        Draw(SpriteStageZero(s), prim, start, count, indices);
        return;
    }
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    if (cap_.file) CapStep(s.rt);
    if (DepthCopy(s)) return;
    Assembled& a = asm_;
    {
        PerfScope perf_assemble{g_perf.assemble};
        if (!Assemble(s, prim, start, count, indices, a)) return;
    }
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    if (!BindTargets(s)) return;

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) return;

    // shaders and states
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    k.sprite = sh.sprite;
    const Program* prog = EnsureProgram(k, sh.vs_code, sh.ps_code);
    if (!prog->vs) return;
    ctx_->VSSetShader(prog->vs.Get(), nullptr, 0);
    ctx_->GSSetShader(prog->gs.Get(), nullptr, 0);
    ctx_->PSSetShader(prog->ps.Get(), nullptr, 0);
    ctx_->IASetInputLayout(prog->layout.Get());
    ApplyStates(s, k);

    // constants, texture views and samplers, viewport and scissor
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    UploadConstants(s, sh, k, stages, size, c);
    if (cap_.file) {
        WriteDrawLine(cap_, s, {prim, count, bound_ds_ ? s.zs.data : 0}, k, c);
        CaptureTextures(s, stages);
        EndDrawLine(cap_, s, a, count, c);
        CaptureShaders(cap_, s, a, sh, pk);
    }
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w);

    // geometry, counted while the title runs a visibility test
    uint32_t query = BeginCountedDraw(s);
    DrawGeometry(a);
    EndCountedDraw(query);
}

void D3D11Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        ID3D11ShaderResourceView* srv = stages.srv[i];
        fprintf(cap_.file, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height,
                tg ? " target" : "", srv ? "" : " unbound");
        if (srv && !tg && stages.info[i].kind == kTex2D && cap_.seen.insert(d.data).second) {
            cap_tex_.push_back({srv, d.data, d.width, d.height});
        }
    }
}

// a draw while the title counts visible pixels gets an occlusion query of its own, kNoQuery otherwise
uint32_t D3D11Backend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2]) return kNoQuery;
    uint32_t query = query_next_++ % kQueries;
    ComPtr<ID3D11Query>& q = queries_[query];
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
    if (!q && FAILED(dev_->CreateQuery(&qd, &q))) {
        LOG_FIRST(1, "d3d11: no occlusion queries, visibility tests stay pending");
        return kNoQuery;
    }
    ctx_->Begin(q.Get());
    return query;
}

void D3D11Backend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    ctx_->End(queries_[query].Get());
    counted_.push_back({query, SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale comes out
float D3D11Backend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    return float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
}

// reports whose queries the GPU has finished, in order, written as the console's GPU writes them: time, count, then 0
// for done
void D3D11Backend::DeliverReports() {
    constexpr uint32_t kTime = 0;
    constexpr uint32_t kTimeHigh = 4;
    constexpr uint32_t kCount = 8;
    constexpr uint32_t kStatus = 12;
    while (!reports_.empty()) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) {
            UINT64 n = 0;
            if (ctx_->GetData(queries_[q.index].Get(), &n, sizeof(n), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
            pixels += double(n) / q.samples_per_pixel;
        }
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

// rs: the rects buffer, reused across targets
void D3D11Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    bool whole = rs.size() == 1 && rs[0].left == 0 && rs[0].top == 0 && rs[0].right == LONG(t.w) &&
                 rs[0].bottom == LONG(t.h);
    float col[4] = {};
    uint8_t mask = 0;
    if (!t.depth) {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Color(c.color, col);
        for (int i = 0; t.range != 1 && i < 3; i++) col[i] /= t.range;
        // D3DCLEAR_TARGET_R/G/B/A are bits 4 to 7, Direct3D's write mask has the same order
        mask = uint8_t((c.flags & kClearTarget) >> 4);
    }
    if (!whole || (!t.depth && mask != D3D11_COLOR_WRITE_ENABLE_ALL)) {
        ClearRects(t, c, col, mask, rs);
    } else if (t.depth) {
        UINT cf = ((c.flags & kClearZ) ? D3D11_CLEAR_DEPTH : 0) | ((c.flags & kClearStencil) ? D3D11_CLEAR_STENCIL : 0);
        ctx_->ClearDepthStencilView(t.dsv.Get(), cf, 1 - std::clamp(c.z, 0.0f, 1.0f), UINT8(c.stencil));
    } else {
        ctx_->ClearRenderTargetView(t.rtv.Get(), col);
    }
    t.drawn = true;
}

constexpr uint64_t kRectClearKey = 0xC1EA;

// a quad per rect, scissored to it: the colour channels in mask, or depth and stencil written whatever they hold
void D3D11Backend::ClearRects(Target& t, const ClearArgs& c, const float col[4], uint8_t mask,
                              const std::vector<RECT>& rs) {
    const Pass* pass = EnsurePass(kRectClearKey, kRectClearHlsl, "vs", "ps");
    if (!pass) return;
    float k[8] = {col[0], col[1], col[2], col[3], 1 - std::clamp(c.z, 0.0f, 1.0f)};
    if (t.depth) {
        ctx_->OMSetRenderTargets(0, nullptr, t.dsv.Get());
    } else {
        ID3D11RenderTargetView* rtv = t.rtv.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    }
    bound_rt_ = bound_ds_ = nullptr;
    D3D11_VIEWPORT vp{0, 0, float(t.w), float(t.h), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    BindPass(*pass, k, sizeof(k));
    ctx_->OMSetBlendState(channel_blend_[t.depth ? 0 : mask].Get(), nullptr, 0xFFFFFFFF);
    if (t.depth) {
        uint32_t which = c.flags & (kClearZ | kClearStencil);
        ctx_->OMSetDepthStencilState(clear_depth_[which].Get(), c.stencil & 0xFF);
    }
    for (const RECT& r : rs) {
        if (r.right <= r.left || r.bottom <= r.top) continue;
        ctx_->RSSetScissorRects(1, &r);
        ctx_->Draw(3, 0);
    }
}

void D3D11Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    PerfScope perf_clear{g_perf.clear};
    if (cap_.file) {
        CapStep(s.rt);
        fprintf(cap_.file, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_.i++, s.rt.data, s.zs.data,
                flags, color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    std::vector<RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// one Blit per rect: for formats or scales that differ
void D3D11Backend::CopyByDraw(const Target& src_t, Target& dst_t, const CopyList& l) {
    ID3D11RenderTargetView* rtv = dst_t.rtv.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    float gw = src_t.gw * l.kx, gh = src_t.gh * l.ky;  // the source in the guest pixels the rects use
    float src_scale = float(src_t.w) / gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = std::min(gw, float(dst_t.gw));
        float whole_h = std::min(gh, float(dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D11_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D11_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetScissorRects(1, &scissor);
        float uv[4] = {r.x0 / gw, r.y0 / gh, (r.x1 - r.x0) / gw, (r.y1 - r.y0) / gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        Blit(src_t.srv.Get(), uv, {.alpha = true, .taps = taps, .mul = 1 / dst_t.range, .rtv = dst_t.fmt});
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopySubresourceRegion per rect, clamped to both targets
void D3D11Backend::CopyByRegion(const Target& src_t, Target& dst_t, const CopyList& l) {
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D11_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        ctx_->CopySubresourceRegion(dst_t.res.Get(), 0, dx, dy, 0, src_t.res.Get(), 0, &b);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D11Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points, float src_kx, float src_ky) {
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if (cap_.file) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_.file, "%04u copy %08X %s to %08X\n", cap_.i++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d11: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points, .kx = src_kx, .ky = src_ky};
    if (src_t->fmt != dst_t->fmt || src_kx != 1 || src_ky != 1) {
        // formats or scales differ: drawn across
        CopyByDraw(*src_t, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, *dst_t, l);
    if (cap_.file) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_.i, dst_t->data);
        CapSave(*dst_t, name);
    }
}

void D3D11Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        CapSave(t.srv.Get(), t.w, t.h, 1, name);
    }
    fclose(cap_.file);
    cap_.file = nullptr;
    cap_tex_.clear();
    LOGI("d3d11: frame captured to %s", cap_.dir.string().c_str());
}

void D3D11Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (w == bb_w_ && h == bb_h_) return;
    // the context lets go of the back buffer first
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    bb_rtv_.Reset();
    ctx_->Flush();
    UINT flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(swap_->ResizeBuffers(kSwapBuffers, w, h, kBgra, flags))) LOGE("d3d11: back buffers not resized");
    CreateBackBufferView();
    bb_w_ = w;
    bb_h_ = h;
}

void D3D11Backend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    uint8_t bgra[kRampBytes];
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = bgra + i * 4;
        p[0] = ramp_[kRampBlue + i];
        p[1] = ramp_[kRampGreen + i];
        p[2] = ramp_[kRampRed + i];
        p[3] = 255;
        identity &= ramp_[kRampRed + i] == i && ramp_[kRampGreen + i] == i && ramp_[kRampBlue + i] == i;
    }
    gamma_ = !identity;
    ctx_->UpdateSubresource(lut_.Get(), 0, nullptr, bgra, kRampBytes, 0);
}

void D3D11Backend::BlitFrame(Target& t) {
    // 16:9 when the console reports a widescreen display, else 4:3
    bool wide = app::Widescreen();
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    D3D11_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D11_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetScissorRects(1, &scissor);
    Blit(t.srv.Get(), uv, {.gamma = gamma_});
}

void D3D11Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void D3D11Backend::DrawFrame(Target* t) {
    float black[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(bb_rtv_.Get(), black);
    ID3D11RenderTargetView* bb = bb_rtv_.Get();
    ctx_->OMSetRenderTargets(1, &bb, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t);
    RenderUi();
    constants_held_ = false;
}

void D3D11Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D11Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void D3D11Backend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
    if (cap_.file) {
        FinishCapture(front);
    } else if (g_capture.exchange(false) && CaptureOn()) {
        StartCapture(cap_);
    }
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (ramp_dirty_) UploadGammaRamp();
    DrawFrame(t);
    bool vsync = app::Get().vsync;
    auto perf_swap = PerfClock::now();
    swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    g_perf.swap += PerfMs(perf_swap);
    // a flip model present unbinds the back buffer
    bound_rt_ = bound_ds_ = nullptr;

    DeliverReports();
    presents_++;
    presents_seen_.store(presents_);
    PollLate();
    ReleaseOldTextures();
    CountFps();
    if (diag::Perf()) LogPerf();
    ApplySettings();
}

void D3D11Backend::ApplySettings() {
    if (texturedumper::Update()) pack_textures_.clear();
    float want = app::RenderScale(guest_h_);
    if (std::fabs(want - scale_) <= 1e-4f) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    targets_.clear();
    scale_ = want;
    bound_rt_ = bound_ds_ = nullptr;
}

// the target shrunk with o when it is not guest sized BGRA; res null on failure
D3D11Backend::CopySource D3D11Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    CopySource out{t.res.Get(), nullptr};
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(t.srv.Get(), t.gw, t.gh, o);
        out.res = out.down.Get();
    }
    return out;
}

// filters src into a w x h BGRA target, o.into or a new one
ComPtr<ID3D11Texture2D> D3D11Backend::Shrink(ID3D11ShaderResourceView* src, uint32_t w, uint32_t h,
                                             const ShrinkOptions& o) {
    g_perf.shrinks++;
    ComPtr<ID3D11Texture2D> down = o.into;
    if (!down) {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, w, h);
        td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &down))) return nullptr;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev_->CreateRenderTargetView(down.Get(), nullptr, &rtv))) return nullptr;
    SetBlitTarget(rtv.Get(), w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    // the next draw binds its own targets; down must not stay bound while it is copied from
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    return down;
}

// rows of a mapped staging copy into out, pitch apart
void D3D11Backend::ReadRows(ID3D11Texture2D* staging, uint32_t w, uint32_t h, uint8_t* out, uint32_t pitch) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return;
    const uint8_t* p = static_cast<const uint8_t*>(m.pData);
    for (uint32_t y = 0; y < h; y++) std::memcpy(out + size_t(y) * pitch, p + size_t(y) * m.RowPitch, w * 4);
    ctx_->Unmap(staging, 0);
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D11Backend::ReadBack(ID3D11Texture2D* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, w, h);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> rb;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &rb))) return false;
    ctx_->CopyResource(rb.Get(), res);
    out.resize(size_t(w) * h * 4);
    ReadRows(rb.Get(), w, h, out.data(), w * 4);
    return true;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D11Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D11Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D11Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end() || it->second.px.empty()) return false;
    Late& l = it->second;
    if (presents_seen_.load() - l.px_frame > uint64_t(kLateCopies) + 1) return false;
    if (l.px_seq > l.delivered) {
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, &l.px[size_t(y) * pitch], l.gw * 4);
        l.delivered = l.px_seq;
    }
    return true;
}

// each finished copy into its surface's px, the newest last; never waits
void D3D11Backend::PollLate() {
    std::lock_guard lock(late_lock_);
    for (auto& [data, l] : late_) {
        for (uint32_t k = 0; k < uint32_t(kLateCopies); k++) {
            uint32_t slot = (l.next + k) % kLateCopies;
            if (!l.pending[slot]) continue;
            if (ctx_->GetData(l.done[slot].Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            l.pending[slot] = false;
            if (l.px_seq && l.frame[slot] < l.px_frame) continue;
            l.px.resize(size_t(GuestPitch(l.gw)) * l.gh);
            ReadRows(l.rb[slot].Get(), l.gw, l.gh, l.px.data(), GuestPitch(l.gw));
            l.px_frame = l.frame[slot];
            l.px_seq++;
        }
    }
}

// a surface set for another size starts over
void D3D11Backend::ResetLate(Late& l, const Target& t) {
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// src into the next slot's staging copy, made on first use, with a query that tells when it is done
void D3D11Backend::RecordLateCopy(Late& l, ID3D11Texture2D* src) {
    uint32_t slot = l.next;
    if (!l.rb[slot]) {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, l.gw, l.gh);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &l.rb[slot])) ||
            FAILED(dev_->CreateQuery(&qd, &l.done[slot]))) {
            l.rb[slot].Reset();
            return;
        }
    }
    ctx_->CopyResource(l.rb[slot].Get(), src);
    ctx_->End(l.done[slot].Get());
    l.pending[slot] = true;
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kLateCopies;
}

void D3D11Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) ResetLate(l, t);
    // a copy for a later lock, unless the GPU is a whole ring behind
    if (l.pending[l.next]) return;
    CopySource src = PrepareCopySource(t, {.into = l.down.Get()});
    if (!src.res) return;
    if (src.down) l.down = src.down;
    RecordLateCopy(l, src.res);
}

void D3D11Backend::CapSave(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, float mul, const char* name) {
    ComPtr<ID3D11Texture2D> down = Shrink(srv, w, h, {.mul = mul});
    std::vector<uint8_t> px;
    if (down && ReadBack(down.Get(), w, h, px)) WriteBmp(cap_, name, px, w, h);
}

void D3D11Backend::CapSave(Target& t, const char* name) {
    CapSave(t.srv.Get(), t.gw, t.gh, t.depth ? 1 : t.range, name);
}

// the frame moves to another target: save the one it leaves
void D3D11Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_.rt) return;
    auto it = targets_.find(TargetMapKey(cap_.rt, false));
    if (cap_.rt && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_.i, cap_.rt);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_.rt, true));
    if (cap_.rt && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_.i, cap_.rt);
        CapSave(z->second, name);
    }
    cap_.rt = d;
}

void D3D11Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateD3D11Backend() { return std::make_unique<D3D11Backend>(); }

}  // namespace gpu
