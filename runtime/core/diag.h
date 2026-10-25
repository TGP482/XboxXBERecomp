#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

// diagnostics switched on by environment variables, read once
namespace diag {

// XBE_PERF set: perf log lines once a second and the read-backs named
inline bool Perf() {
    static const bool on = std::getenv("XBE_PERF") != nullptr;
    return on;
}

// XBE_TRACE_KERNEL set: kernel calls are logged with their first arguments, return value and return address, up to
// the number it holds per export (200 when it is not a number), so a stall shows the call that never returned
inline unsigned TraceKernel() {
    static const unsigned limit = [] {
        const char* v = std::getenv("XBE_TRACE_KERNEL");
        if (!v) return 0u;
        unsigned n = unsigned(std::strtoul(v, nullptr, 10));
        return n ? n : 200u;
    }();
    return limit;
}

// XBE_CAPTURE_DIR: where F3 saves a frame, empty: F3 does not capture
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
#ifdef _WIN32
        const wchar_t* v = _wgetenv(L"XBE_CAPTURE_DIR");
#else
        const char* v = std::getenv("XBE_CAPTURE_DIR");
#endif
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return dir;
}

// XBE_CAPTURE_EVERY: with XBE_CAPTURE_DIR, captures a frame every this many seconds without F3, 0 or unset: never
inline double CaptureEvery() {
    static const double seconds = [] {
        const char* v = std::getenv("XBE_CAPTURE_EVERY");
        return v ? std::strtod(v, nullptr) : 0.0;
    }();
    return seconds;
}

// XBE_CAPTURE_STEP: in a capture, the bound render target is also saved after every this many draws, to see what each
// draw added; 0 or unset: never
inline unsigned CaptureStep() {
    static const unsigned n = [] {
        const char* v = std::getenv("XBE_CAPTURE_STEP");
        return v ? unsigned(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    return n;
}

// XBE_CAPTURE_RANGE: "first-last", the draw numbers of a capture that XBE_CAPTURE_STEP saves targets after (every draw of
// the range, whatever the step), to look at a few draws without a gigabyte of targets; unset: the step alone
inline bool CaptureInRange(unsigned draw) {
    static const std::pair<unsigned, unsigned> range = [] {
        const char* v = std::getenv("XBE_CAPTURE_RANGE");
        unsigned a = 0, b = 0;
        if (v && std::sscanf(v, "%u-%u", &a, &b) == 2) return std::make_pair(a, b);
        return std::make_pair(1u, 0u);
    }();
    return draw >= range.first && draw <= range.second;
}

// XBE_MULTISAMPLE_TYPE: a D3DMULTISAMPLE_TYPE in hex that Direct3D_CreateDevice uses in place of the title's, to try
// the other frame buffer models on a title; 0 or unset: the title's. Buffers the title supplies keep their size, and
// D3DRS_MULTISAMPLEMODE stays at the type's own, whatever the title's state blocks set for the type it chose.
inline unsigned MultiSampleType() {
    static const unsigned type = [] {
        const char* v = std::getenv("XBE_MULTISAMPLE_TYPE");
        return v ? unsigned(std::strtoul(v, nullptr, 16)) : 0u;
    }();
    return type;
}

// XBE_BACKBUFFER_SCALE: "x,y" (or one factor for both) that D3DDevice_SetBackBufferScale uses in place of the
// title's; 0 or unset: the title's
inline bool BackBufferScale(float& x, float& y) {
    static const float* scale = [] {
        static float s[2] = {};
        const char* v = std::getenv("XBE_BACKBUFFER_SCALE");
        if (!v) return static_cast<const float*>(nullptr);
        char* end = nullptr;
        s[0] = std::strtof(v, &end);
        s[1] = end && *end == ',' ? std::strtof(end + 1, nullptr) : s[0];
        return s[0] > 0 && s[1] > 0 ? static_cast<const float*>(s) : nullptr;
    }();
    if (!scale) return false;
    x = scale[0];
    y = scale[1];
    return true;
}

// XBE_TRACE_READS: reads of the files whose console path holds this text (any case) are logged with their offset and
// length (with XBE_VERBOSE in a release build), to see which parts of a file a title streams, an XACT wave bank's waves
// say; unset: none
inline const std::string& TraceReads() {
    static const std::string text = [] {
        const char* v = std::getenv("XBE_TRACE_READS");
        std::string s = v ? v : "";
        for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }();
    return text;
}

}  // namespace diag
