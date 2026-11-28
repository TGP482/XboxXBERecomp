#pragma once
#include <dxgiformat.h>

#include <cstdint>
#include <vector>

namespace gpu {

// what every backend keeps of a console surface it draws into: its console and host size and format, what the CPU
// saw of it
struct TargetBase {
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    float range = 1;         // colour held divided by it until the extension resolves the target
    uint64_t tag = ~0ull;    // the extension's
};

}  // namespace gpu
