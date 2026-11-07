// BinkGetSummary divides bytes read * 1000 by the read time in ms + 1. Host reads take under a millisecond,
// the time stays 0 and the quotient of a large movie overflows the 32 bit div. Raise the time just enough.
#include <mutex>
#include <unordered_set>

#include "xdk/hle.h"

using namespace kernel;

X86_EXTERN_FUNC(GUEST_ORIG(BinkGetSummary));
X86_FUNC(GUEST(BinkGetSummary)) {
    uint32_t bink = R32(ctx.esp + 4);
    if (bink) {
        // 0x134 bytes read, 0x13C read time in ms
        uint64_t scaled = uint64_t(R32(bink + 0x134)) * 1000;
        uint32_t time = R32(bink + 0x13C);
        if (scaled >> 32 > time) W32(bink + 0x13C, uint32_t(scaled >> 32));
    }
    GUEST_ORIG(BinkGetSummary)(ctx, base);
}

// Movies step one frame per game frame, timed by the console's 30 Hz present. Above 30 fps a frame is decoded
// and advanced only once BinkWait, which follows the movie's own rate and audio, says it is due.
namespace {

constexpr hle::Va kBinkWait = SYM(BinkWait);
std::mutex g_held_lock;
std::unordered_set<uint32_t> g_held;  // movies whose frame was not decoded this game frame

}  // namespace

X86_EXTERN_FUNC(GUEST_ORIG(BinkDoFrame));
X86_FUNC(GUEST(BinkDoFrame)) {
    uint32_t bink = R32(ctx.esp + 4);
    if (bink && CallGuest(ctx, kBinkWait, {bink})) {
        std::lock_guard<std::mutex> lock(g_held_lock);
        g_held.insert(bink);
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    GUEST_ORIG(BinkDoFrame)(ctx, base);
}

X86_EXTERN_FUNC(GUEST_ORIG(BinkNextFrame));
X86_FUNC(GUEST(BinkNextFrame)) {
    uint32_t bink = R32(ctx.esp + 4);
    {
        std::lock_guard<std::mutex> lock(g_held_lock);
        if (g_held.erase(bink)) {
            ctx.esp += 8;
            return;
        }
    }
    GUEST_ORIG(BinkNextFrame)(ctx, base);
}
