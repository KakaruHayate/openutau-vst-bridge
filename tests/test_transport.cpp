#include "transport.h"

#include <doctest.h>

#include <cmath>
#include <cstdint>

using bridge::BlockSeconds;
using bridge::BlockStartFrame;
using bridge::IsPlaying;

namespace {

/// A transport the way a host fills one in. Only the four fields this code reads are named: the
/// rest of the struct has changed shape across CLAP 1.x, and zero-init covers it.
clap_event_transport_t Transport(uint32_t flags) {
    clap_event_transport_t transport{};
    transport.header.size = sizeof(transport);
    transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    transport.header.type = CLAP_EVENT_TRANSPORT;
    transport.flags = flags;
    return transport;
}

clap_sectime SecTime(double seconds) {
    return static_cast<clap_sectime>(std::llround(seconds * CLAP_SECTIME_FACTOR));
}

clap_beattime BeatTime(double beats) {
    return static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));
}

}  // namespace

TEST_CASE("A seconds timeline is the position, converted to frames at the host's rate") {
    clap_event_transport_t transport = Transport(CLAP_TRANSPORT_HAS_SECONDS_TIMELINE);
    transport.song_pos_seconds = SecTime(2.5);

    double seconds = 0.0;
    REQUIRE(BlockSeconds(&transport, &seconds));
    CHECK(seconds == doctest::Approx(2.5));

    int64_t frame = -1;
    REQUIRE(BlockStartFrame(&transport, 48000.0, &frame));
    CHECK(frame == 120000);
    REQUIRE(BlockStartFrame(&transport, 44100.0, &frame));
    CHECK(frame == 110250);
}

TEST_CASE("A host with only a musical timeline is followed through its tempo") {
    // Four beats at 120 bpm is two seconds, and two seconds is where the part's window is read.
    clap_event_transport_t transport =
        Transport(CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TEMPO);
    transport.song_pos_beats = BeatTime(4.0);
    transport.tempo = 120.0;

    double seconds = 0.0;
    REQUIRE(BlockSeconds(&transport, &seconds));
    CHECK(seconds == doctest::Approx(2.0));

    int64_t frame = -1;
    REQUIRE(BlockStartFrame(&transport, 44100.0, &frame));
    CHECK(frame == 88200);
}

TEST_CASE("Seconds win over beats when the host offers both") {
    // Deliberately disagreeing: a host that has resolved its own tempo map has already done this
    // conversion, and its answer is the one that matches what it will record.
    clap_event_transport_t transport = Transport(CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
                                                CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                                                CLAP_TRANSPORT_HAS_TEMPO);
    transport.song_pos_seconds = SecTime(1.0);
    transport.song_pos_beats = BeatTime(8.0);  // 4 s at this tempo, and not what is wanted.
    transport.tempo = 120.0;

    double seconds = 0.0;
    REQUIRE(BlockSeconds(&transport, &seconds));
    CHECK(seconds == doctest::Approx(1.0));
}

TEST_CASE("A position the host does not give is not guessed at") {
    double seconds = 1234.0;
    int64_t frame = 4321;

    SUBCASE("no transport at all") {
        CHECK_FALSE(BlockSeconds(nullptr, &seconds));
        CHECK_FALSE(BlockStartFrame(nullptr, 48000.0, &frame));
    }
    SUBCASE("a transport with neither timeline") {
        clap_event_transport_t transport = Transport(CLAP_TRANSPORT_IS_PLAYING);
        CHECK_FALSE(BlockSeconds(&transport, &seconds));
    }
    SUBCASE("beats without a tempo to convert them by") {
        clap_event_transport_t transport = Transport(CLAP_TRANSPORT_HAS_BEATS_TIMELINE);
        transport.song_pos_beats = BeatTime(4.0);
        CHECK_FALSE(BlockSeconds(&transport, &seconds));
    }
    SUBCASE("a tempo of zero, which would divide rather than convert") {
        clap_event_transport_t transport =
            Transport(CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TEMPO);
        transport.song_pos_beats = BeatTime(4.0);
        transport.tempo = 0.0;
        CHECK_FALSE(BlockSeconds(&transport, &seconds));
    }
    SUBCASE("before activate, when there is no rate to convert at") {
        clap_event_transport_t transport = Transport(CLAP_TRANSPORT_HAS_SECONDS_TIMELINE);
        transport.song_pos_seconds = SecTime(2.5);
        CHECK_FALSE(BlockStartFrame(&transport, 0.0, &frame));
    }
    // Nothing was written on the way out, so a caller that ignores the return value cannot pick up
    // a position that was never reported.
    CHECK(seconds == 1234.0);
    CHECK(frame == 4321);
}

TEST_CASE("A frame position is rounded to the nearest frame, not truncated") {
    clap_event_transport_t transport = Transport(CLAP_TRANSPORT_HAS_SECONDS_TIMELINE);
    transport.song_pos_seconds = SecTime(22050.6 / 44100.0);

    int64_t frame = 0;
    REQUIRE(BlockStartFrame(&transport, 44100.0, &frame));
    CHECK(frame == 22051);
}

TEST_CASE("A position before the timeline's start stays negative") {
    // Pre-roll: the mixer's positions are signed all the way through for this, and clamping here
    // would slide the first part of the project forward by the pre-roll's length.
    clap_event_transport_t transport = Transport(CLAP_TRANSPORT_HAS_SECONDS_TIMELINE);
    transport.song_pos_seconds = SecTime(-0.5);

    int64_t frame = 0;
    REQUIRE(BlockStartFrame(&transport, 44100.0, &frame));
    CHECK(frame == -22050);
}

TEST_CASE("Playing is what the host says it is") {
    clap_event_transport_t stopped = Transport(CLAP_TRANSPORT_HAS_SECONDS_TIMELINE);
    clap_event_transport_t playing =
        Transport(CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING);

    CHECK_FALSE(IsPlaying(nullptr));  // No transport is not a start.
    CHECK_FALSE(IsPlaying(&stopped));
    CHECK(IsPlaying(&playing));
}
