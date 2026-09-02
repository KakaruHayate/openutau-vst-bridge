#pragma once

/*
 * Line and frame syntax for both planes (PROTOCOL.md §5). Parsing mirrors
 * `DawTransport.DispatchControl` and `DawAudio.TryParseFrameHeader` on the OpenUtau side,
 * including their tolerances, so a line either side accepts is accepted here too.
 */

#include <cstdint>
#include <string>

namespace bridge {

/// A received line starting with this introduces binary, not control (PROTOCOL.md §5.2).
inline constexpr const char *kFramePrefix = "audio ";

enum class ControlKind {
    /// Malformed or a header this version does not know. Logged and survived, never fatal (§8).
    Unknown,
    /// Bare `close`, no payload.
    Close,
    Request,
    Response,
    Notification,
};

struct ControlLine {
    ControlKind kind = ControlKind::Unknown;
    /// Set for Request and Response.
    std::string uuid;
    /// Request kind or notification kind.
    std::string name;
    /// Raw JSON text after the first space. Empty when the line carried none.
    std::string payload;
};

/// Splits one control line (newline already stripped) into header parts and raw payload.
ControlLine ParseControlLine(const std::string &line);

bool IsFrameHeader(const std::string &line);

/// Parses `audio <hash> <length>`. Rejects anything `int.TryParse(NumberStyles.None)` would
/// reject on the other side — no sign, no whitespace, nothing past int32 — because a
/// malformed length would desynchronize the stream past recovery (§8).
bool TryParseFrameHeader(const std::string &line, std::string *hash, int32_t *length);

/// Includes the trailing newline: unlike a control line, the newline is part of the binary
/// frame and the payload follows it verbatim.
std::string BuildFrameHeader(const std::string &hash, size_t length);

/// These return the line without its newline; the writer appends one.
std::string BuildRequestLine(const std::string &uuid, const std::string &kind, const std::string &json);
std::string BuildResponseLine(const std::string &uuid, const std::string &json);
std::string BuildNotificationLine(const std::string &kind, const std::string &json);

/// A v4-shaped random identifier for outbound requests. Only has to be unique per
/// connection and free of ':' and ' '.
std::string NewUuid();

}  // namespace bridge
