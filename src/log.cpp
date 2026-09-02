#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace bridge {
namespace {

const char *LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Warning:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Info:
        default:
            return "info";
    }
}

// Two threads reaching fprintf at once would interleave mid-line on some platforms, and a
// torn log line is worse than a slow one.
std::mutex &LogMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

void LogMessage(LogLevel level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    {
        std::lock_guard<std::mutex> guard(LogMutex());
        std::fprintf(stderr, "[OpenUtau Bridge] %s: ", LevelName(level));
        std::vfprintf(stderr, format, args);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }
    va_end(args);
}

}  // namespace bridge
