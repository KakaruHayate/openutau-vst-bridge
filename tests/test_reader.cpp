#include "reader.h"

#include "scripted_stream.h"

#include <doctest.h>

#include <string>
#include <vector>

using bridge::FrameReader;
using bridge::ReadStatus;
using bridge::test::ScriptedStream;

namespace {

std::vector<uint8_t> Bytes(const std::string &text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

}  // namespace

TEST_CASE("A control line is read whole however the stream splits it") {
    ScriptedStream stream;
    stream.PushBytesSingly("notification:ping {}\n");
    FrameReader reader(&stream);

    std::string line;
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line == "notification:ping {}");
}

TEST_CASE("Several lines buffered by one read are returned one at a time") {
    ScriptedStream stream;
    stream.PushBytes("first\nsecond\nthird\n");
    FrameReader reader(&stream);

    std::string line;
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line == "first");
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line == "second");
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line == "third");
}

TEST_CASE("CRLF and empty lines are tolerated") {
    ScriptedStream stream;
    stream.PushBytes("close\r\n\r\n\n");
    FrameReader reader(&stream);

    std::string line;
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line == "close");
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line.empty());
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line.empty());
}

TEST_CASE("A line longer than the initial buffer grows it rather than truncating") {
    // A USTX document arrives as one control line, so this is the ordinary case, not an edge.
    std::string huge(100000, 'x');
    ScriptedStream stream;
    stream.PushBytes(huge + "\n");
    FrameReader reader(&stream);

    std::string line;
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line.size() == huge.size());
    CHECK(line == huge);
}

TEST_CASE("A timeout keeps the partial line for the next call") {
    // This is what lets the caller ping and expire pulls on a quiet connection: a timeout
    // must not cost the bytes already received.
    ScriptedStream stream;
    stream.PushBytes("par");
    FrameReader reader(&stream);

    std::string line;
    CHECK(reader.ReadLine(&line, 20) == ReadStatus::Timeout);

    stream.PushBytes("tial\n");
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line == "partial");
}

TEST_CASE("A close between messages is an end of stream, mid-line it is a protocol error") {
    SUBCASE("between messages") {
        ScriptedStream stream;
        stream.PushBytes("close\n");
        stream.PushEnd();
        FrameReader reader(&stream);

        std::string line;
        REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
        CHECK(reader.ReadLine(&line, 500) == ReadStatus::EndOfStream);
    }
    SUBCASE("mid-line") {
        ScriptedStream stream;
        stream.PushBytes("half a li");
        stream.PushEnd();
        FrameReader reader(&stream);

        std::string line;
        CHECK(reader.ReadLine(&line, 500) == ReadStatus::ProtocolError);
        CHECK(reader.Error().find("mid-line") != std::string::npos);
    }
}

TEST_CASE("A failed read is a protocol error") {
    ScriptedStream stream;
    stream.PushFailure();
    FrameReader reader(&stream);

    std::string line;
    CHECK(reader.ReadLine(&line, 500) == ReadStatus::ProtocolError);
    CHECK(!reader.Error().empty());
}

TEST_CASE("A payload buffered behind its header is not lost") {
    // The reason the two planes share one buffer: the read that delivered the header very
    // likely delivered the payload with it.
    std::string payload = "\x01\x02\x03\x04\x05\x06\x07\x08";
    ScriptedStream stream;
    stream.PushBytes("audio 42 8\n" + payload);
    FrameReader reader(&stream);

    std::string line;
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line == "audio 42 8");

    std::vector<uint8_t> read;
    REQUIRE(reader.ReadExactly(8, &read) == ReadStatus::Ok);
    CHECK(read == Bytes(payload));
}

TEST_CASE("A payload is reassembled across arbitrary read boundaries") {
    std::string payload(3000, '\x7f');
    ScriptedStream stream;
    stream.PushBytes("audio 42 3000\n");
    stream.PushBytes(payload.substr(0, 1));
    stream.PushBytes(payload.substr(1, 2500));
    stream.PushTimeout();
    stream.PushBytes(payload.substr(2501));
    FrameReader reader(&stream);

    std::string line;
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    std::vector<uint8_t> read;
    REQUIRE(reader.ReadExactly(payload.size(), &read) == ReadStatus::Ok);
    CHECK(read == Bytes(payload));
}

TEST_CASE("A zero-length payload reads without touching the stream") {
    // §6.1 allows an empty part; it must not turn into a wait for bytes that never come.
    ScriptedStream stream;
    FrameReader reader(&stream);

    std::vector<uint8_t> read{1, 2, 3};
    REQUIRE(reader.ReadExactly(0, &read) == ReadStatus::Ok);
    CHECK(read.empty());
}

TEST_CASE("A close mid-payload is a protocol error, never a short payload") {
    // Accepting a short read would hand half a part to the mixer and leave the stream
    // position lost for every frame after it (§8).
    ScriptedStream stream;
    stream.PushBytes("audio 42 8\n");
    stream.PushBytes("half");
    stream.PushEnd();
    FrameReader reader(&stream);

    std::string line;
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    std::vector<uint8_t> read;
    CHECK(reader.ReadExactly(8, &read) == ReadStatus::ProtocolError);
    CHECK(reader.Error().find("4 of 8") != std::string::npos);
}

TEST_CASE("Control lines resume after a payload") {
    // Length-explicit: the first two bytes are NUL, so a C-string constructor would stop dead.
    std::string payload("\x00\x00\x80\x3f", 4);  // 1.0f, little-endian.
    ScriptedStream stream;
    stream.PushBytes("audio 42 4\n" + payload + "notification:ping {}\n");
    FrameReader reader(&stream);

    std::string line;
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line == "audio 42 4");
    std::vector<uint8_t> read;
    REQUIRE(reader.ReadExactly(4, &read) == ReadStatus::Ok);
    REQUIRE(reader.ReadLine(&line, 500) == ReadStatus::Ok);
    CHECK(line == "notification:ping {}");
}
