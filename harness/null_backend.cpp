// headless renderer: counts frames and draws, logs draw lists, draws chosen frames with the reference renderer
// REF=N: every Nth frame drawn on the CPU into guest memory and saved as cap/ref_<frame>.tga
// CAP=N: every Nth frame's draws listed in cap/draws_<frame>.txt
// snap in the run folder: the next two frames drawn and saved as snap_1.tga and snap_2.tga, then snap is removed
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "harness.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "reference_renderer.h"

namespace gpu {

namespace {

class NullBackend final : public Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        LOGI("renderer: headless %ux%u", w, h);
        std::filesystem::create_directories(app::ExeDir() / "cap");
        return true;
    }
    void Clear(const State& s, uint32_t n, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        if (drawing_) reference::Clear(s, n, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        draws_++;
        if (drawing_) reference::Draw(s, prim, start, count, indices);
        if (list_) List(s, prim, start, count);
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t n, uint32_t rects, uint32_t points) override {
        if (drawing_) reference::Copy(src, dst, n, rects, points);
        else CopySurfaceMemory(src, dst, n, rects, points);
    }
    void Present(const Surface& front) override {
        if (list_) {
            std::fclose(list_);
            list_ = nullptr;
        }
        if (draws_ == last_draws_) empty_++;
        last_draws_ = draws_;
        if (drawing_ && !snap_) reference::WriteTga(front, File("cap/ref_%05llu.tga", frames_).c_str());
        // snap asks for the next two frames drawn
        std::string snap = (app::ExeDir() / "snap").string();
        if (snap_ == 2) {
            reference::WriteTga(front, (app::ExeDir() / "snap_2.tga").string().c_str());
            std::remove(snap.c_str());
            snap_ = 0;
        } else if (snap_ == 1) {
            reference::WriteTga(front, (app::ExeDir() / "snap_1.tga").string().c_str());
            snap_ = 2;
        } else if (FILE* f = std::fopen(snap.c_str(), "r")) {
            std::fclose(f);
            snap_ = 1;
        }
        static const long every = harness::Env("REF"), cap = harness::Env("CAP");
        drawing_ = snap_ || (every && (frames_ + 1) % every == 0);
        if (drawing_) reference::BeginFrame();
        harness::g_frames = frames_ + 1;
        if (++frames_ % 60 == 0) LOGI("renderer: frame %llu, %llu draws, %llu empty frames", frames_, draws_, empty_);
        if (cap && frames_ % cap == 0) list_ = std::fopen(File("cap/draws_%05llu.txt", frames_).c_str(), "w");
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t, uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}

private:
    static std::string File(const char* fmt, unsigned long long n) {
        char name[64];
        std::snprintf(name, sizeof name, fmt, n);
        return (app::ExeDir() / name).string();
    }

    // one draw's state, textures, transforms, declaration and first vertex
    void List(const State& s, uint32_t prim, uint32_t start, uint32_t count) {
        FILE* f = list_;
        std::fprintf(f, "DRAW prim=%u n=%u vs=%X ps=%X rt=%08X tex0=%08X light=%u blend=%u %X %X cw=%X at=%u z=%u\n", prim, count,
                     s.vs_handle, s.ps ? s.ps->handle : 0, s.rt.data, s.textures[0], s.rs[kRsLighting], s.simple[kMBlendEnable >> 2],
                     s.simple[kMSrcBlend >> 2], s.simple[kMDestBlend >> 2], s.simple[kMColorWriteEnable >> 2],
                     s.simple[kMAlphaTestEnable >> 2], s.rs[kRsZEnable]);
        const auto& t0 = s.tss[0];
        TextureDesc d;
        if (s.textures[0] && Describe(s.textures[0], d))
            std::fprintf(f, "  tex %ux%u fmt %X cop %u %X %X aop %u %X %X\n", d.width, d.height, d.format, t0[kTssColorOp],
                         t0[kTssColorArg1], t0[kTssColorArg2], t0[kTssAlphaOp], t0[kTssAlphaArg1], t0[kTssAlphaArg2]);
        std::fprintf(f, "  ttf %X tci %X addr %u %u t16", t0[kTssTextureTransformFlags], t0[kTssTexCoordIndex], t0[kTssAddressU],
                     t0[kTssAddressV]);
        auto matrix = [&](const char* name, uint32_t which) {
            std::fprintf(f, "%s", name);
            for (float v : s.transforms[which]) std::fprintf(f, " %g", v);
        };
        matrix("", 16);
        matrix("\n  world", 256);
        matrix("\n  view", 2);
        matrix("\n  proj", 3);
        std::fprintf(f, "\n");
        if (s.vs) {
            std::fprintf(f, "  decl");
            for (int r = 0; r < 16; r++) {
                const auto& in = s.vs->inputs[size_t(r)];
                if (in.stream >= 0) std::fprintf(f, " v%d:%d+%u/%X", r, in.stream, in.offset, in.type);
            }
            std::fprintf(f, "\n");
        }
        if (s.streams[0].vb) {
            // the vertex buffer's Data at +4 is a physical address
            uint32_t data = mem::VirtualFromPhysical(kernel::R32(s.streams[0].vb + 4)) + start * s.streams[0].stride;
            std::fprintf(f, "  v0 stride %u:", s.streams[0].stride);
            for (uint32_t i = 0; i < s.streams[0].stride / 4 && i < 10; i++) {
                uint32_t v = kernel::R32(data + i * 4);
                float x;
                std::memcpy(&x, &v, 4);
                std::fprintf(f, " %g(%08X)", x, v);
            }
            std::fprintf(f, "\n");
        }
    }

    unsigned long long frames_ = 0, draws_ = 0, last_draws_ = 0, empty_ = 0;
    bool drawing_ = false;
    int snap_ = 0;
    FILE* list_ = nullptr;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<NullBackend>(); }

}  // namespace gpu
