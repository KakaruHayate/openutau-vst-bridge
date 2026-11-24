#include "binding.h"

#include "messages.h"
#include "timeline.h"

#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using bridge::AudioStore;
using bridge::Binding;
using bridge::PartLayout;
using bridge::Resolution;
using bridge::TrackInfo;

namespace {

TrackInfo Track(const std::string &id, const std::string &name) {
    TrackInfo track;
    track.id = id;
    track.name = name;
    return track;
}

PartLayout Part(int trackNo, const std::string &trackId, const std::string &hash) {
    PartLayout part;
    part.trackNo = trackNo;
    part.trackId = trackId;
    part.startMs = 0.0;
    part.endMs = 100.0;
    part.audioHash = hash;
    return part;
}

std::vector<uint8_t> Wire(float value) {
    float pair[2] = {value, value};
    std::vector<uint8_t> bytes(sizeof(pair));
    std::memcpy(bytes.data(), pair, sizeof(pair));
    return bytes;
}

}  // namespace

TEST_CASE("Without ids the position is the binding, as with an older OpenUtau") {
    Binding binding;
    binding.trackNo = 1;
    std::vector<TrackInfo> tracks = {Track("", "A"), Track("", "B")};

    Resolution resolved = bridge::Resolve(&binding, tracks);

    CHECK(resolved.trackNo == 1);
    CHECK_FALSE(resolved.adopted);
    CHECK(binding.trackId.empty());
}

TEST_CASE("A binding without an id adopts the id at its position") {
    Binding binding;
    binding.trackNo = 1;
    std::vector<TrackInfo> tracks = {Track("ta", "A"), Track("tb", "B")};

    Resolution resolved = bridge::Resolve(&binding, tracks);

    CHECK(resolved.trackNo == 1);
    CHECK(resolved.adopted);
    CHECK(binding.trackId == "tb");
}

TEST_CASE("A bound track is followed when tracks are reordered") {
    Binding binding;
    binding.trackId = "tb";
    binding.trackNo = 1;
    std::vector<TrackInfo> reordered = {Track("tb", "B"), Track("tc", "C"), Track("ta", "A")};

    Resolution resolved = bridge::Resolve(&binding, reordered);

    CHECK(resolved.trackNo == 0);
    CHECK_FALSE(resolved.adopted);
    CHECK(binding.trackNo == 0);
    CHECK(binding.trackId == "tb");
}

TEST_CASE("A deleted track unbinds without falling back to a neighbour, and comes back") {
    Binding binding;
    binding.trackId = "tb";
    binding.trackNo = 1;

    Resolution gone = bridge::Resolve(&binding, {Track("ta", "A"), Track("tc", "C")});
    CHECK(gone.trackNo == -1);
    // The id and the last position stay, so an undo re-resolves to the same track.
    CHECK(binding.trackId == "tb");
    CHECK(binding.trackNo == 1);

    Resolution back =
        bridge::Resolve(&binding, {Track("ta", "A"), Track("tb", "B"), Track("tc", "C")});
    CHECK(back.trackNo == 1);
}

TEST_CASE("A position past the end does not adopt anything") {
    Binding binding;
    binding.trackNo = 5;

    Resolution resolved = bridge::Resolve(&binding, {Track("ta", "A")});

    CHECK(resolved.trackNo == -1);
    CHECK(binding.trackId.empty());
}

TEST_CASE("The state keeps the 1.1 layout and appends the id") {
    Binding binding;
    binding.trackId = "tabcdefghij";
    binding.trackNo = 2;

    std::vector<uint8_t> bytes = bridge::EncodeState(binding);

    // What plugin 1.1 reads: "OUBR", version 1, the position. It ignores what follows.
    REQUIRE(bytes.size() > 12);
    CHECK(std::memcmp(bytes.data(), "OUBR", 4) == 0);
    uint32_t version = 0;
    int32_t trackNo = 0;
    std::memcpy(&version, bytes.data() + 4, 4);
    std::memcpy(&trackNo, bytes.data() + 8, 4);
    CHECK(version == 1);
    CHECK(trackNo == 2);

    Binding decoded;
    REQUIRE(bridge::DecodeState(bytes, &decoded));
    CHECK(decoded.trackId == "tabcdefghij");
    CHECK(decoded.trackNo == 2);
}

TEST_CASE("A 1.1 state decodes to a position without an id") {
    Binding binding;
    binding.trackNo = 3;
    std::vector<uint8_t> bytes = bridge::EncodeState(binding);
    REQUIRE(bytes.size() == 12);

    Binding decoded;
    decoded.trackId = "stale";
    REQUIRE(bridge::DecodeState(bytes, &decoded));
    CHECK(decoded.trackNo == 3);
    CHECK(decoded.trackId.empty());
}

TEST_CASE("Foreign or truncated state is refused") {
    Binding decoded;
    CHECK_FALSE(bridge::DecodeState({}, &decoded));
    CHECK_FALSE(bridge::DecodeState({'X', 'X', 'X', 'X', 1, 0, 0, 0, 0, 0, 0, 0}, &decoded));
    std::vector<uint8_t> newer = bridge::EncodeState(Binding{});
    newer[4] = 2;  // A version this build does not know.
    CHECK_FALSE(bridge::DecodeState(newer, &decoded));
    // A cut-off id is ignored, not misread: the position alone still loads.
    Binding binding;
    binding.trackId = "tabcdefghij";
    binding.trackNo = 1;
    std::vector<uint8_t> cut = bridge::EncodeState(binding);
    cut.resize(cut.size() - 3);
    REQUIRE(bridge::DecodeState(cut, &decoded));
    CHECK(decoded.trackNo == 1);
    CHECK(decoded.trackId.empty());
}

TEST_CASE("A layout with ids is filtered by id, so a lagging layout plays no neighbour") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    REQUIRE(store.Insert("mine", Wire(0.5f)));
    REQUIRE(store.Insert("theirs", Wire(-0.25f)));
    // The track list already moved "tb" to position 0, the layout still says position 1.
    std::vector<TrackInfo> tracks = {Track("tb", "B"), Track("ta", "A")};
    std::vector<PartLayout> layout = {Part(0, "ta", "theirs"), Part(1, "tb", "mine")};

    auto timeline = bridge::BuildTimeline(layout, tracks, store, 0, 44100.0, "tb");

    REQUIRE(timeline->parts.size() == 1);
    CHECK(timeline->parts[0].clip->left[0] == 0.5f);
}

TEST_CASE("A binding with an id still plays by position against a layout without ids") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    REQUIRE(store.Insert("mine", Wire(0.5f)));
    std::vector<TrackInfo> tracks = {Track("", "A"), Track("", "B")};
    std::vector<PartLayout> layout = {Part(1, "", "mine")};

    auto timeline = bridge::BuildTimeline(layout, tracks, store, 1, 44100.0, "tb");

    REQUIRE(timeline->parts.size() == 1);
}

TEST_CASE("Track and part ids are read, and bindTrack and bindingChanged have their shape") {
    std::vector<TrackInfo> tracks;
    REQUIRE(bridge::ParseTracksNotification(
        R"({"tracks":[{"id":"ta","name":"A","volume":0,"pan":0,"muted":false}]})", &tracks));
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0].id == "ta");

    std::vector<PartLayout> parts;
    REQUIRE(bridge::ParsePartLayoutRequest(
        R"({"parts":[{"trackNo":0,"trackId":"ta","startMs":0,"endMs":1,"audioHash":"1"}]})",
        &parts));
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].trackId == "ta");

    std::string trackId;
    CHECK(bridge::ParseBindTrackRequest(R"({"trackId":"tb"})", &trackId));
    CHECK(trackId == "tb");
    CHECK_FALSE(bridge::ParseBindTrackRequest(R"({"trackId":""})", &trackId));
    CHECK_FALSE(bridge::ParseBindTrackRequest(R"({})", &trackId));

    CHECK(bridge::BuildBindingChangedPayload("tb", "Vocals") ==
          R"({"hostTrackName":"Vocals","trackId":"tb"})");
    CHECK(bridge::BuildBindingChangedPayload("", "") ==
          R"({"hostTrackName":null,"trackId":null})");
}
