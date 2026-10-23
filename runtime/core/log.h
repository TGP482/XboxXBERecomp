#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>

namespace logx {

// a release build writes Report and Error lines only
enum class Level { Debug, Info, Warn, Error, Report };

// release: dir/prefix.log, carried on when append; debug: dir/prefix_NNN.log per run, newest 30 kept
void Init(const std::string& dir, const std::string& prefix, bool append = false);
void Write(Level lv, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Flush();
const std::string& Path();

// called with the text of every line before the level is checked, so a release build without XBE_VERBOSE shows an
// observer the lines it does not write; from any thread, and it must not log or wait on a lock a logging thread holds.
// nullptr removes it.
void SetObserver(void (*observer)(const char* message));

#ifdef NDEBUG
inline constexpr bool kVerbose = false;
#else
inline constexpr bool kVerbose = true;
#endif

}  // namespace logx

#define LOGD(...) ::logx::Write(::logx::Level::Debug, __VA_ARGS__)
#define LOGI(...) ::logx::Write(::logx::Level::Info, __VA_ARGS__)
#define LOGW(...) ::logx::Write(::logx::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::logx::Write(::logx::Level::Error, __VA_ARGS__)
#define LOGR(...) ::logx::Write(::logx::Level::Report, __VA_ARGS__)

// logs only the first n times a call site is reached
#define LOG_FIRST(n, ...)                                  \
    do {                                                   \
        static std::atomic<int> _c{0};                     \
        if (_c.fetch_add(1, std::memory_order_relaxed) < (n)) LOGI(__VA_ARGS__); \
    } while (0)
