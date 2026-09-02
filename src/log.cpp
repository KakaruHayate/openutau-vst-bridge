#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

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

int ProcessId() {
#if defined(_WIN32)
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

/// A DAW gives a plugin no console, so stderr goes nowhere and a failed connection would have to
/// be diagnosed blind. One file per process — every instance in one host shares it, two hosts do
/// not — opened once and left open, since a log that reopens per line loses lines under load.
/// Null when the file could not be opened, which is not worth reporting anywhere.
std::FILE *LogFile() {
    static std::FILE *file = [] () -> std::FILE * {
        std::error_code error;
        std::filesystem::path directory = std::filesystem::temp_directory_path(error) / "OpenUtau";
        if (error) {
            return nullptr;
        }
        std::filesystem::create_directories(directory, error);
        std::filesystem::path path =
            directory / ("bridge-" + std::to_string(ProcessId()) + ".log");
#if defined(_WIN32)
        // The wide form, because a path under a non-ASCII user name would not survive the
        // active code page.
        return _wfopen(path.c_str(), L"a");
#else
        return std::fopen(path.c_str(), "a");
#endif
    }();
    return file;
}

// Two threads reaching fprintf at once would interleave mid-line on some platforms, and a
// torn log line is worse than a slow one.
std::mutex &LogMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

void LogMessage(LogLevel level, const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    // Formatted once, then written wherever it can be seen: a va_list cannot be walked twice.
    int written = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written < 0) {
        return;
    }

    std::lock_guard<std::mutex> guard(LogMutex());
    std::fprintf(stderr, "[OpenUtau Bridge] %s: %s\n", LevelName(level), line);
    std::fflush(stderr);
    if (std::FILE *file = LogFile(); file != nullptr) {
        std::fprintf(file, "[OpenUtau Bridge] %s: %s\n", LevelName(level), line);
        std::fflush(file);  // A crash must not take the line that explains it.
    }
#if defined(_WIN32)
    // Visible in DebugView and in a debugger's output window, which is where a plugin developer
    // looks first on Windows.
    std::string debug = std::string("[OpenUtau Bridge] ") + LevelName(level) + ": " + line + "\n";
    ::OutputDebugStringA(debug.c_str());
#endif
}

}  // namespace bridge
