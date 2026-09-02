#include <doctest.h>

#include "frame.h"

#include <string>

using bridge::BuildFrameHeader;
using bridge::BuildNotificationLine;
using bridge::BuildRequestLine;
using bridge::BuildResponseLine;
using bridge::ControlKind;
using bridge::IsFrameHeader;
using bridge::NewUuid;
using bridge::ParseControlLine;
using bridge::TryParseFrameHeader;

TEST_CASE("a bare close line is recognized") {
    auto line = ParseControlLine("close");
    CHECK(line.kind == ControlKind::Close);
    CHECK(line.payload.empty());
}

TEST_CASE("requests split at the first colon only") {
    // §5.1: request:<uuid>:<kind>, and the kind may itself contain ':'.
    auto line = ParseControlLine("request:abc-123:updatePartLayoutV2:beta {\"parts\":[]}");
    REQUIRE(line.kind == ControlKind::Request);
    CHECK(line.uuid == "abc-123");
    CHECK(line.name == "updatePartLayoutV2:beta");
    CHECK(line.payload == "{\"parts\":[]}");
}

TEST_CASE("responses and notifications carry their own identifiers") {
    auto response = ParseControlLine("response:abc-123 {\"success\":true}");
    REQUIRE(response.kind == ControlKind::Response);
    CHECK(response.uuid == "abc-123");
    CHECK(response.payload == "{\"success\":true}");

    auto notification = ParseControlLine("notification:ping {}");
    REQUIRE(notification.kind == ControlKind::Notification);
    CHECK(notification.name == "ping");
    CHECK(notification.payload == "{}");
}

TEST_CASE("a payloadless control line parses with an empty payload") {
    auto line = ParseControlLine("notification:ping");
    REQUIRE(line.kind == ControlKind::Notification);
    CHECK(line.name == "ping");
    CHECK(line.payload.empty());
}

TEST_CASE("malformed control headers are unknown rather than fatal") {
    for (const char *text : {"request:", "request:onlyuuid", "request::kind", "request:uuid:",
                             "response:", "notification:", "garbage", "", "audio"}) {
        CHECK(ParseControlLine(text).kind == ControlKind::Unknown);
    }
}

TEST_CASE("control lines round-trip through their builders") {
    auto request = ParseControlLine(BuildRequestLine("u1", "getAudio", "{\"hash\":\"7\"}"));
    REQUIRE(request.kind == ControlKind::Request);
    CHECK(request.uuid == "u1");
    CHECK(request.name == "getAudio");
    CHECK(request.payload == "{\"hash\":\"7\"}");

    auto response = ParseControlLine(BuildResponseLine("u1", "{\"success\":false}"));
    REQUIRE(response.kind == ControlKind::Response);
    CHECK(response.uuid == "u1");

    auto notification = ParseControlLine(BuildNotificationLine("playbackStarted", "{}"));
    REQUIRE(notification.kind == ControlKind::Notification);
    CHECK(notification.name == "playbackStarted");
}

TEST_CASE("data frame headers are told apart from control lines") {
    CHECK(IsFrameHeader("audio 7 4"));
    CHECK_FALSE(IsFrameHeader("audioX 7 4"));
    // The prefix includes its space, so a kind that merely starts with "audio" is control.
    CHECK_FALSE(IsFrameHeader("notification:audioReady {}"));
}

TEST_CASE("frame headers parse and rebuild") {
    std::string hash;
    int32_t length = 0;
    REQUIRE(TryParseFrameHeader("audio 13507256038857166760 3528000", &hash, &length));
    CHECK(hash == "13507256038857166760");
    CHECK(length == 3528000);
    CHECK(BuildFrameHeader(hash, static_cast<size_t>(length)) ==
          "audio 13507256038857166760 3528000\n");
}

TEST_CASE("a zero-length frame is well-formed") {
    std::string hash;
    int32_t length = -1;
    REQUIRE(TryParseFrameHeader("audio 0 0", &hash, &length));
    CHECK(length == 0);
}

TEST_CASE("frame headers that could desynchronize the stream are rejected") {
    std::string hash;
    int32_t length = 0;
    // No length, extra field, signed or fractional length, unparseable hash, and one past
    // int32 — all of which the other side's int.TryParse(NumberStyles.None) refuses too.
    for (const char *text : {"audio 7", "audio 7 4 5", "audio 7 -4", "audio 7 4.0",
                             "audio abc 4", "audio 7 2147483648", "audio  7 4"}) {
        CHECK_FALSE(TryParseFrameHeader(text, &hash, &length));
    }
    CHECK(TryParseFrameHeader("audio 7 2147483647", &hash, &length));
}

TEST_CASE("generated uuids are distinct and contain no separator characters") {
    std::string first = NewUuid();
    std::string second = NewUuid();
    CHECK(first != second);
    CHECK(first.size() == 36);
    CHECK(first.find(':') == std::string::npos);
    CHECK(first.find(' ') == std::string::npos);
    CHECK(first.find('\n') == std::string::npos);
}
