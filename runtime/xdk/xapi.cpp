// XAPI functions that talk to hardware: controller ports and the hard disk cache partition
#include <algorithm>
#include <chrono>

#include "core/input.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

// configs written before the SDK knew the name
#ifndef SYM_XInputGetCapabilities
#define SYM_XInputGetCapabilities FFFFFC00
#define HAS_XInputGetCapabilities 0
#endif

using namespace kernel;

namespace {

// XPP device type records: CurrentConnected, ChangeConnected, PreviousConnected
constexpr uint32_t kGamepadType = SYM(XPP_GamepadType);
constexpr uint32_t kMemoryUnitType = SYM(XPP_MemoryUnitType);

std::mutex g_ports_lock;
bool g_poller = false;

void UpdatePorts() {
    std::lock_guard<std::mutex> lock(g_ports_lock);
    uint32_t cur = 0;
    for (int p = 0; p < 4; p++)
        if (input::Poll(p).connected) cur |= 1u << p;
    uint32_t old = R32(kGamepadType + XDK_XPP_Current);
    if (cur != old) {
        W32(kGamepadType + XDK_XPP_Changed, R32(kGamepadType + XDK_XPP_Changed) | (cur ^ old));
        W32(kGamepadType + XDK_XPP_Current, cur);
        LOGI("xapi: controller ports now %X", cur);
    }
}

struct Handle {
    uint32_t port;
    uint32_t packet;
    input::Pad last;
};
std::unordered_map<uint32_t, Handle> g_handles;

// verbose: every few seconds, how the title drove each pad's motors since the last report; the mean is each motor's
// speed averaged over the time, as a share of full
struct RumbleLog {
    uint32_t calls = 0, changes = 0;
    uint16_t left = 0, right = 0, left_max = 0, right_max = 0;
    double left_area = 0, right_area = 0;  // speed times seconds
    std::chrono::steady_clock::time_point since{};  // the area's end
};
RumbleLog g_rumble[4];

void AddRumbleArea(RumbleLog& r, std::chrono::steady_clock::time_point now) {
    if (r.since != std::chrono::steady_clock::time_point{}) {
        double s = std::chrono::duration<double>(now - r.since).count();
        r.left_area += r.left * s;
        r.right_area += r.right * s;
    }
    r.since = now;
}

void LogRumble(uint32_t port, uint16_t left, uint16_t right) {
    static auto start = std::chrono::steady_clock::now();
    static auto next = start;
    auto now = std::chrono::steady_clock::now();
    RumbleLog& r = g_rumble[port];
    AddRumbleArea(r, now);
    r.calls++;
    if (left != r.left || right != r.right) r.changes++;
    r.left = left;
    r.right = right;
    r.left_max = std::max(r.left_max, left);
    r.right_max = std::max(r.right_max, right);
    if (now < next) return;
    next = now + std::chrono::seconds(3);
    double window = std::max(std::chrono::duration<double>(now - start).count(), 1e-3);
    start = now;
    for (uint32_t p = 0; p < 4; p++) {
        RumbleLog& l = g_rumble[p];
        AddRumbleArea(l, now);
        if (l.calls)
            LOGI("xapi: rumble port %u: %u sets, %u changes, left (low) up to %u mean %.1f%%, right (high) up to %u mean "
             "%.1f%%, now %u %u", p, l.calls, l.changes, l.left_max, l.left_area / window / 655.35, l.right_max,
             l.right_area / window / 655.35, l.left, l.right);
        l = {0, 0, l.left, l.right, 0, 0, 0, 0, now};
    }
}

}  // namespace

HLE_STDCALL(XMountUtilityDrive, 1) {
    CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    return 1;
}

HLE_STDCALL(XInitDevices, 2) {
    XDK_REQUIRE("xapi", XDK_KEY(XPP_Current), XDK_KEY(XPP_Changed), XDK_KEY(XPP_Previous), XDK_DATA(XPP_GamepadType));
    W32(kGamepadType + XDK_XPP_Current, 0);
    W32(kGamepadType + XDK_XPP_Changed, 0);
    W32(kGamepadType + XDK_XPP_Previous, 0);
    if constexpr (HAS_XPP_MemoryUnitType != 0) {  // titles without memory unit support have no record
        W32(kMemoryUnitType + XDK_XPP_Current, 0);
        W32(kMemoryUnitType + XDK_XPP_Changed, 0);
        W32(kMemoryUnitType + XDK_XPP_Previous, 0);
    }
    UpdatePorts();
    if (!g_poller) {
        g_poller = true;
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                UpdatePorts();
            }
        }).detach();
    }
    return 0;
}

HLE_STDCALL(XInputOpen, 4) {
    uint32_t type = a[0], port = a[1];
    if (type != kGamepadType || port > 3) return 0;
    uint32_t h = mem::AllocSystem(16);
    std::lock_guard<std::mutex> lock(g_ports_lock);
    g_handles[h] = {port, 0, {}};
    return h;
}

HLE_STDCALL(XInputClose, 1) {
    std::lock_guard<std::mutex> lock(g_ports_lock);
    g_handles.erase(a[0]);
    mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(XInputGetState, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;  // ERROR_INVALID_HANDLE
        port = it->second.port;
    }
    input::Pad p = input::Poll(int(port));
    if (!p.connected) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
    std::lock_guard<std::mutex> lock(g_ports_lock);
    Handle& h = g_handles[a[0]];
    if (std::memcmp(&p, &h.last, sizeof(p))) h.packet++;
    h.last = p;
    uint32_t s = a[1];
    W32(s, h.packet);  // XINPUT_STATE: packet, buttons, 8 analog buttons, thumbs
    W16(s + 4, p.buttons);
    std::memcpy(HostPtr(s + 6), p.analog, 8);
    W16(s + 14, uint16_t(p.lx));
    W16(s + 16, uint16_t(p.ly));
    W16(s + 18, uint16_t(p.rx));
    W16(s + 20, uint16_t(p.ry));
    return 0;
}

// XINPUT_CAPABILITIES: subtype, reserved, the gamepad's buttons, 8 analog buttons and thumbs, then its 2 motors, each
// with every bit set: the controller has them all
HLE_STDCALL(XInputGetCapabilities, 2) {
    constexpr uint32_t kCapsBytes = 25;
    constexpr uint8_t kSubtypeGamepad = 1;  // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;  // ERROR_INVALID_HANDLE
        port = it->second.port;
    }
    if (!input::Poll(int(port)).connected) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
    uint32_t c = a[1];
    std::memset(HostPtr(c), 0xFF, kCapsBytes);
    G<uint8_t>(c) = kSubtypeGamepad;
    W16(c + 1, 0);
    W16(c + 3, 0x00FF);  // the 8 digital buttons
    return 0;
}

HLE_STDCALL(XInputSetState, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;
        port = it->second.port;
    }
    uint32_t fb = a[1];
    // XINPUT_FEEDBACK_HEADER is dwStatus, hEvent and 58 reserved bytes, the rumble follows: the left (low frequency)
    // motor's speed, then the right (high frequency) motor's
    uint16_t left = R16(fb + 66), right = R16(fb + 68);
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        LogRumble(port, left, right);
    }
    input::Rumble(int(port), left, right);
    W32(fb, 0);
    if (uint32_t ev = R32(fb + 4))
        if (ObjectRef o = FromHandle(ev)) SetEvent(*o);
    return 997;  // ERROR_IO_PENDING, already complete
}

// A recompiled image launches through the XAPI, then a process restart into its executable; any other
// (the dashboard, updates) is refused and the title stays in its menu.
X86_EXTERN_FUNC(GUEST_ORIG(XLaunchNewImageA));
X86_FUNC(GUEST(XLaunchNewImageA)) {
    std::string path = ReadCString(R32(ctx.esp + 4));
    if (ImageExe(path)) {
        GUEST_ORIG(XLaunchNewImageA)(ctx, base);
        return;
    }
    LOGW("xapi: launch of %s refused, it is not recompiled", path.c_str());
    Return(ctx, 2, 2);  // ERROR_FILE_NOT_FOUND
}
