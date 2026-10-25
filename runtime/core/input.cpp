// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report, and the
// keyboard as a controller
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/input.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};

// The console's motors run until they are set again. SDL resends a rumble every 2 s while it lasts, for pads that stop
// one on their own after a few seconds (the DualShock 4), but only a rumble with a duration: each gets the longest
// SDL takes, renewed at half of it while the title leaves the motors running.
constexpr uint32_t kRumbleMs = 0xFFFF;  // SDL_MAX_RUMBLE_DURATION_MS
struct Motors {
    uint16_t left = 0, right = 0;
    uint64_t renewed = 0;  // SDL_GetTicks
};
Motors g_motors[4];

// SDL's driver for a controller, from its GUID's signature byte (SDL_joystick.c, SDL_IsJoystickHIDAPI and friends)
const char* DriverName(SDL_GUID guid) {
    switch (guid.data[14]) {
        case 'h': return "HIDAPI";
        case 'w': return "Windows.Gaming.Input";
        case 'g': return "GameInput";
        case 'r': return "RawInput";
        case 'x': return "XInput";
        case 'm': return "MFi";
        case 'v': return "virtual";
        default: return "other driver";
    }
}

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    int port = int(slot - g_pads) + 1;
    if (!*slot) {
        LOGI("input: controller %d failed to open: %s", port, SDL_GetError());
        return;
    }
    const char* name = SDL_GetGamepadName(*slot);
    SDL_JoystickConnectionState link = SDL_GetGamepadConnectionState(*slot);
    SDL_PropertiesID props = SDL_GetGamepadProperties(*slot);
    LOGI("input: controller %d plugged in: %s (%04X:%04X, %s, %s, %s)", port, name ? name : "",
         SDL_GetGamepadVendor(*slot), SDL_GetGamepadProduct(*slot), DriverName(SDL_GetGamepadGUIDForID(id)),
         link == SDL_JOYSTICK_CONNECTION_WIRED ? "wired" : link == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless" : "link unknown",
         SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "rumble" : "no rumble");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// XBE_INPUT_SCRIPT: timed presses on a scripted controller in port 1 for unattended runs, "seconds:button[:held]"
// separated by commas, e.g. "30:start,33:a:0.2" (held defaults to 0.15 s). Buttons: up down left right start back ls
// rs a b x y black white lt rt. A stick axis is "seconds:axis=value[:held]" with the axis lx, ly, rx or ry and a
// value from -1 to 1 (x right, y up), e.g. "40:lx=-0.5:2" steers left at half deflection for 2 s; where axis
// entries overlap, the one that started last wins. "seconds:wait=text[:timeout[:button]]" (up to 64) holds the
// script's clock at that time until a log line containing text (any case, no commas or colons) has been written since
// the script began, or the timeout (default 120 s) passes: "30:wait=UserProfile.xml,32:a" presses A two seconds after
// the game opens the profile, however long loading takes. "text*3" waits for the third such line. With a button, the
// hold presses it for 0.15 s every 1.5 s, the first time 0.75 s in, for a screen that takes the first press only
// when it is ready. With a script, physical controllers are ignored, so a pad in use elsewhere cannot steer the
// run.
struct Press {
    double at, until;
    uint16_t buttons;   // Pad::buttons bits
    uint8_t analog;     // Pad::analog index + 1, 0 for none
    uint8_t axis;       // stick axis: 1 lx, 2 ly, 3 rx, 4 ry, 0 for none
    int16_t value;
};
struct Wait {
    double at, timeout;
    std::string text;   // lower case
    int need;           // lines with the text before the wait passes
    Press repeat;       // pressed now and then while the wait is open, buttons and analog 0 for none
};
std::vector<Press> g_script;
std::vector<Wait> g_waits;  // by at
bool g_scripted = false;
uint64_t g_script_start = 0;

// the script's clock stops at a wait until a line with its text has been logged (a line logged since the script began
// counts, so a wait for something the game has already done passes at once); the state is guarded by g_script_lock,
// except that the log observer, which runs on whichever thread logs, only reads g_waits and counts into g_heard
constexpr size_t kMaxWaits = 64;
std::atomic<int> g_heard[kMaxWaits];  // lines logged with wait i's text
std::mutex g_script_lock;
size_t g_wait_next = 0;       // the first wait the clock has not passed
double g_held_total = 0;      // seconds the clock has been held
double g_hold_began = -1;     // real seconds when the open wait began, -1 when none is open

void OnLog(const char* message) {
    std::string line = message;
    for (char& c : line) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (size_t i = 0; i < g_waits.size(); i++)
        if (line.find(g_waits[i].text) != std::string::npos) g_heard[i]++;
}

// the script's clock; the open wait's repeating press while its pulse is on goes to *pulse
double ScriptTime(const Press** pulse) {
    std::lock_guard lock(g_script_lock);
    double real = double(SDL_GetTicks() - g_script_start) / 1000.0;
    *pulse = nullptr;
    for (;;) {
        double now = real - g_held_total;
        if (g_wait_next >= g_waits.size()) return now;
        const Wait& w = g_waits[g_wait_next];
        if (g_hold_began < 0) {
            if (now < w.at) return now;
            g_hold_began = real - (now - w.at);
        }
        bool heard = g_heard[g_wait_next] >= w.need;
        bool timed_out = real - g_hold_began >= w.timeout;
        if (!heard && !timed_out) {
            if (std::fmod(real - g_hold_began + 0.75, 1.5) < 0.15) *pulse = &w.repeat;
            return w.at;
        }
        g_held_total += real - g_hold_began;
        g_hold_began = -1;
        g_wait_next++;
        if (!heard) LOGW("input: no log line with \"%s\" in %.0f s, going on", w.text.c_str(), w.timeout);
    }
}

void LoadScript() {
    const char* env = std::getenv("XBE_INPUT_SCRIPT");
    if (!env || !*env) return;
    static const std::pair<const char*, int> kNames[] = {
        {"up", 0}, {"down", 1}, {"left", 2}, {"right", 3}, {"start", 4}, {"back", 5}, {"ls", 6}, {"rs", 7},
        {"a", 8}, {"b", 9}, {"x", 10}, {"y", 11}, {"black", 12}, {"white", 13}, {"lt", 14}, {"rt", 15}};
    static const char* kAxes[] = {"lx", "ly", "rx", "ry"};
    std::string all = env;
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t end = all.find(',', pos);
        std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? all.size() + 1 : end + 1;
        double at = 0, held = 0.15, value = 0;
        char name[16] = {};
        if (size_t colon = item.find(':'); colon != std::string::npos && item.compare(colon + 1, 5, "wait=") == 0) {
            std::string text = item.substr(colon + 6);
            double timeout = 120;
            Press repeat{0, 0, 0, 0, 0, 0};
            if (size_t sep = text.find(':'); sep != std::string::npos) {
                std::string rest = text.substr(sep + 1);
                text.resize(sep);
                timeout = std::atof(rest.c_str());
                if (size_t sep2 = rest.find(':'); sep2 != std::string::npos) {
                    std::string button = rest.substr(sep2 + 1);
                    for (auto [label, index] : kNames) {
                        if (button != label) continue;
                        if (index < 8) repeat.buttons = uint16_t(1 << index);
                        else repeat.analog = uint8_t(index - 8 + 1);
                    }
                }
            }
            int need = 1;
            if (size_t star = text.rfind('*'); star != std::string::npos) {
                need = std::max(std::atoi(text.c_str() + star + 1), 1);
                text.resize(star);
            }
            for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (!text.empty() && g_waits.size() < kMaxWaits && std::sscanf(item.c_str(), "%lf", &at) == 1)
                g_waits.push_back({at, timeout, text, need, repeat});
            continue;
        }
        if (std::sscanf(item.c_str(), "%lf:%2[a-z]=%lf:%lf", &at, name, &value, &held) >= 3) {
            for (int i = 0; i < 4; i++) {
                if (std::strcmp(kAxes[i], name) != 0) continue;
                int16_t v = int16_t(std::clamp(value, -1.0, 1.0) * 32767.0);
                g_script.push_back({at, at + held, 0, 0, uint8_t(i + 1), v});
            }
            continue;
        }
        held = 0.15;
        int n = std::sscanf(item.c_str(), "%lf:%15[a-z]:%lf", &at, name, &held);
        if (n < 2) continue;
        for (auto [label, index] : kNames) {
            if (std::strcmp(label, name) != 0) continue;
            Press press{at, at + held, 0, 0, 0, 0};
            if (index < 8) press.buttons = uint16_t(1 << index);
            else press.analog = uint8_t(index - 8 + 1);
            g_script.push_back(press);
        }
    }
    std::stable_sort(g_waits.begin(), g_waits.end(), [](const Wait& a, const Wait& b) { return a.at < b.at; });
    if (!g_waits.empty()) logx::SetObserver(OnLog);
    g_scripted = true;
    g_script_start = SDL_GetTicks();
    LOGI("input: scripted controller in port 1, %zu presses, %zu waits; physical controllers ignored", g_script.size(),
         g_waits.size());
}

Pad ScriptedPad() {
    Pad p;
    p.connected = true;
    const Press* pulse;
    double now = ScriptTime(&pulse);
    if (pulse) {
        p.buttons |= pulse->buttons;
        if (pulse->analog) p.analog[pulse->analog - 1] = 0xFF;
    }
    double started[4] = {-1, -1, -1, -1};
    for (const Press& press : g_script) {
        if (now < press.at || now >= press.until) continue;
        if (press.axis) {
            int16_t* sticks[4] = {&p.lx, &p.ly, &p.rx, &p.ry};
            if (press.at < started[press.axis - 1]) continue;
            started[press.axis - 1] = press.at;
            *sticks[press.axis - 1] = press.value;
            continue;
        }
        p.buttons |= press.buttons;
        if (press.analog) p.analog[press.analog - 1] = 0xFF;
    }
    return p;
}

// The keyboard as the controller in port 1, read while the game's window has the focus: it stands in for a controller
// when none is plugged in and adds to one when it is. The game window is no SDL window, so the keys are read from the
// system. W and S are the right and left triggers (accelerate and brake in a racing game), A and D the left stick
// across, the arrows the D-pad, I J K L the right stick, Space A (one key confirms everything, the title screen too),
// Escape or Backspace B, X X, Y Y, Q white, E black, Enter start, Tab back.
void AddKeyboard(Pad& p) {
    p.connected = true;
#ifdef _WIN32
    HWND window = static_cast<HWND>(app::Window());
    bool focus = window && GetForegroundWindow() == window;
    static std::atomic<int> had_focus{-1};
    if (had_focus.exchange(focus) != int(focus))
        LOGI(focus ? "input: the keyboard plays in port 1 (the game window has the focus)"
                   : "input: the keyboard is off (another window has the focus)");
    if (!focus) return;
    auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    // D-pad up, down, left, right, start, back
    constexpr int kButtons[6] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_TAB};
    for (int i = 0; i < 6; i++) p.buttons |= uint16_t(down(kButtons[i]) << i);
    // A B X Y black white, then the triggers
    bool analog[8] = {down(VK_SPACE), down(VK_ESCAPE) || down(VK_BACK), down('X'), down('Y'), down('E'), down('Q'),
                      down('S'), down('W')};
    for (int i = 0; i < 8; i++) {
        if (analog[i]) p.analog[i] = 0xFF;
    }
    auto axis = [&](int16_t& value, int negative, int positive) {
        if (down(negative) != down(positive)) value = down(positive) ? 32767 : -32768;
    };
    axis(p.lx, 'A', 'D');
    axis(p.rx, 'J', 'L');
    axis(p.ry, 'K', 'I');
#endif
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
    for (int i = 0; i < 4; i++) {
        Motors& m = g_motors[i];
        if (g_pads[i] && (m.left || m.right) && now - m.renewed >= kRumbleMs / 2) {
            SDL_RumbleGamepad(g_pads[i], m.left, m.right, kRumbleMs);
            m.renewed = now;
        }
    }
}

}  // namespace

void Init() {
    if (g_sdl) return;  // started already for the game code window
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    LoadScript();
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    Pad p;
    if (g_scripted) {
        if (port == 0) p = ScriptedPad();
    } else {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
        }
        if (port == 0) AddKeyboard(p);
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

uint16_t HeldOnAnyController() {
    constexpr std::pair<SDL_GamepadButton, uint16_t> kFaceButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},  {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    std::lock_guard lock(g_lock);
    if (g_scripted) {
        Pad p = ScriptedPad();
        uint16_t held = 0;
        for (int i = 0; i < 4; i++)
            if (p.analog[i]) held |= uint16_t(0x1000 << i);
        return held;
    }
    UpdatePads();
    uint16_t held = 0;
    for (SDL_Gamepad* pad : g_pads)
        for (auto [button, mask] : kFaceButtons)
            if (pad && SDL_GetGamepadButton(pad, button)) held |= mask;
    Pad keys;
    AddKeyboard(keys);
    for (int i = 0; i < 4; i++)
        if (keys.analog[i]) held |= uint16_t(0x1000 << i);
    return held;
}

// the speeds reach SDL unscaled; a scripted run ignores the physical controllers, and leaves their motors alone too
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port < 0 || port >= 4) return;
    g_motors[port] = {left, right, SDL_GetTicks()};
    if (g_scripted || !g_pads[port]) return;
    if (!SDL_RumbleGamepad(g_pads[port], left, right, kRumbleMs))
        LOG_FIRST(4, "input: rumble on controller %d failed: %s", port + 1, SDL_GetError());
}

}  // namespace input
