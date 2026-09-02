#include "frame.h"

#include "hash.h"

#include <array>
#include <cstdio>
#include <limits>
#include <random>
#include <string_view>

namespace bridge {
namespace {

constexpr std::string_view kRequestPrefix = "request:";
constexpr std::string_view kResponsePrefix = "response:";
constexpr std::string_view kNotificationPrefix = "notification:";

/// Same strictness as the hash parse, but bounded by int32 because the other side reads the
/// length with `int.TryParse`.
bool TryParseLength(const std::string &text, int32_t *length) {
    uint64_t value = 0;
    if (!TryParseHash(text, &value)) {
        return false;
    }
    if (value > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    *length = static_cast<int32_t>(value);
    return true;
}

}  // namespace

ControlLine ParseControlLine(const std::string &line) {
    ControlLine result;
    if (line == "close") {
        result.kind = ControlKind::Close;
        return result;
    }
    size_t space = line.find(' ');
    std::string header = space == std::string::npos ? line : line.substr(0, space);
    if (space != std::string::npos) {
        result.payload = line.substr(space + 1);
    }
    if (header.starts_with(kResponsePrefix)) {
        result.uuid = header.substr(kResponsePrefix.size());
        if (!result.uuid.empty()) {
            result.kind = ControlKind::Response;
        }
    } else if (header.starts_with(kNotificationPrefix)) {
        result.name = header.substr(kNotificationPrefix.size());
        if (!result.name.empty()) {
            result.kind = ControlKind::Notification;
        }
    } else if (header.starts_with(kRequestPrefix)) {
        // request:<uuid>:<kind> — the kind may itself contain ':', so split only at the first.
        std::string rest = header.substr(kRequestPrefix.size());
        size_t colon = rest.find(':');
        if (colon != std::string::npos) {
            std::string uuid = rest.substr(0, colon);
            std::string kind = rest.substr(colon + 1);
            if (!uuid.empty() && !kind.empty()) {
                result.uuid = std::move(uuid);
                result.name = std::move(kind);
                result.kind = ControlKind::Request;
            }
        }
    }
    if (result.kind == ControlKind::Unknown) {
        result.uuid.clear();
        result.name.clear();
    }
    return result;
}

bool IsFrameHeader(const std::string &line) {
    return line.starts_with(kFramePrefix);
}

bool TryParseFrameHeader(const std::string &line, std::string *hash, int32_t *length) {
    if (!IsFrameHeader(line)) {
        return false;
    }
    std::string rest = line.substr(std::string_view(kFramePrefix).size());
    size_t space = rest.find(' ');
    if (space == std::string::npos) {
        return false;
    }
    std::string hashText = rest.substr(0, space);
    std::string lengthText = rest.substr(space + 1);
    // A third field would be a different frame syntax, not one with a trailing extra.
    if (lengthText.find(' ') != std::string::npos) {
        return false;
    }
    uint64_t parsedHash = 0;
    int32_t parsedLength = 0;
    if (!TryParseHash(hashText, &parsedHash) || !TryParseLength(lengthText, &parsedLength)) {
        return false;
    }
    *hash = std::move(hashText);
    *length = parsedLength;
    return true;
}

std::string BuildFrameHeader(const std::string &hash, size_t length) {
    return std::string(kFramePrefix) + hash + " " + std::to_string(length) + "\n";
}

std::string BuildRequestLine(const std::string &uuid, const std::string &kind, const std::string &json) {
    return std::string(kRequestPrefix) + uuid + ":" + kind + " " + json;
}

std::string BuildResponseLine(const std::string &uuid, const std::string &json) {
    return std::string(kResponsePrefix) + uuid + " " + json;
}

std::string BuildNotificationLine(const std::string &kind, const std::string &json) {
    return std::string(kNotificationPrefix) + kind + " " + json;
}

std::string NewUuid() {
    static thread_local std::mt19937_64 engine(std::random_device{}());
    uint64_t high = engine();
    uint64_t low = engine();
    // Version 4, variant 1 — cosmetic here, but it keeps the value recognizable in logs
    // beside the Guid.NewGuid() strings OpenUtau emits.
    high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    std::array<char, 37> text{};
    std::snprintf(text.data(), text.size(), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(high >> 32),
                  static_cast<unsigned>((high >> 16) & 0xFFFF),
                  static_cast<unsigned>(high & 0xFFFF),
                  static_cast<unsigned>(low >> 48),
                  static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFULL));
    return std::string(text.data());
}

}  // namespace bridge
