#pragma once
// What the Direct3D backends share: formats and sizes, guest state read into draw constants and pipeline keys,
// target bookkeeping, the perf diagnostic
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcommon.h>
#include <dxgi1_6.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/pass_hlsl.h"
#include "gpu/shaders.h"
#include "gpu/target.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"

namespace gpu {

constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t);

struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t raw = 0;
    uint64_t lates = 0;
};

extern Perf g_perf;
struct PerfScope {
    double& acc;
    bool on = diag::Perf();
    PerfClock::time_point t = on ? PerfClock::now() : PerfClock::time_point{};
    ~PerfScope() {
        if (on) acc += PerfMs(t);
    }
};

// Depth is reversed in a 32 bit float buffer: 1 at the near clip, 0 at the far one, where a float keeps its finest
// steps; a 24 bit buffer storing z / w lost the steps that told distant surfaces apart
constexpr DXGI_FORMAT kDepthTypeless = DXGI_FORMAT_R32G8X24_TYPELESS;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
constexpr DXGI_FORMAT kDepthRead = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
constexpr float kDepthFar = 0;

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t sprite;  // DrawShaders::sprite
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
};

// the last targets EnsureTarget and FindTarget marked used, so target_used_ is written once a frame each
struct UsedTarget {
    uint32_t data = 0;
    uint64_t frame = UINT64_MAX;
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    bool sprite = false;           // a program's points drawn as point sprites (SpriteHlsl)
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

// what the constants and the pixel shader read of a draw's four texture stages
struct StageInfo {
    TexInfo info[4];
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
};

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

// diagnostics
bool CaptureOn();  // F3 captures only with diag::CaptureDir set
void LogPerf();    // once a second with diag::Perf

// guest values
float Bits(uint32_t v);
void Color(uint32_t argb, float* o);
// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207 as Direct3D 11 and 12 number them, from 1
uint32_t CmpFunc(uint32_t v);
DXGI_FORMAT HostFormat(Host h);
uint8_t ShadowDepthKind(uint32_t format);
uint32_t GuestPitch(uint32_t width);
uint32_t SimpleState(const State& s, uint32_t method);
TexKind TextureKind(const TextureDesc& d);
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette);

// targets
uint64_t MemSig(uint32_t data, uint32_t total);
bool Changed(const TargetBase& t);
bool LatchCpuWrite(TargetBase& t);
std::vector<uint8_t> LatchCpuChanges(TargetBase& t);
uint64_t TargetMapKey(uint32_t data, bool depth);
RECT ScaledRect(const TargetBase& t, int x0, int y0, int x1, int y1);
RECT ViewportScissorRect(const State& s, uint32_t gw, float w, float h);

// shaders and pipeline keys
DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count);
void EnsureProgramHash(const State& s);
uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh);
std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh);
std::string SpriteHlsl();
void KeyBlend(const State& s, PsoKey& k);
void KeyDepthStencil(const State& s, const Assembled& a, const TargetBase* ds, PsoKey& k);
void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k);
bool UnitTexcoords(const Assembled& a);
bool AnyStageWraps(const State& s);
D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a);
bool SpriteOnStage3(const State& s, uint32_t prim);
State SpriteStageZero(const State& s);

// draw constants
void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c);
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c);
void FillCombinerConstants(const State& s, DrawConstants& c);
void FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c);
// target_scale: host pixels per console pixel of the bound target
float RenderTargetShift(const State& s, const DrawShaders& sh, float target_scale);
// rt_shift: RenderTargetShift, for the stages that read a target
void FillTextureConstants(const State& s, const StageInfo& stages, float rt_shift, DrawConstants& c);
void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c);
void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c);

// CopyRects' guest lists
GuestRect ReadGuestRect(uint32_t rects, uint32_t i);
GuestPoint ReadGuestPoint(uint32_t points, uint32_t i);
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i);
UINT ScaleUnsigned(int32_t v, float sc);

}  // namespace gpu
