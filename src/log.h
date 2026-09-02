#pragma once

/*
 * Diagnostics for the worker side. Hosts capture stderr, which is the only output channel
 * a plugin can rely on without pulling in a logging dependency.
 *
 * Never call this from the audio thread: it locks and it formats.
 */

namespace bridge {

enum class LogLevel {
    Info,
    Warning,
    Error,
};

/// printf-style. Appends its own newline.
void LogMessage(LogLevel level, const char *format, ...);

}  // namespace bridge

#define BRIDGE_INFO(...) ::bridge::LogMessage(::bridge::LogLevel::Info, __VA_ARGS__)
#define BRIDGE_WARN(...) ::bridge::LogMessage(::bridge::LogLevel::Warning, __VA_ARGS__)
#define BRIDGE_ERROR(...) ::bridge::LogMessage(::bridge::LogLevel::Error, __VA_ARGS__)
