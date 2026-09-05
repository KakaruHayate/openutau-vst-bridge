#include "timeline.h"

#include <doctest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using bridge::AudioClip;
using bridge::AudioClipPtr;
using bridge::AudioStore;
using bridge::PartLayout;
using bridge::Timeline;
using bridge::TimelineBox;
using bridge::TimelinePart;
using bridge::TimelinePtr;
using bridge::TrackInfo;

namespace {

AudioClipPtr Clip(std::vector<float> left, std::vector<float> right) {
    auto clip = std::make_shared<AudioClip>();
    clip->left = std::move(left);
    clip->right = std::move(right);
    return clip;
}

/// 1, 2, 3 … on the left and 10, 20, 30 … on the right: a wrong offset or a swapped channel
/// shows up as a wrong number rather than as a plausible one.
AudioClipPtr Ramp(size_t frames) {
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    for (size_t i = 0; i < frames; i++) {
        left[i] = static_cast<float>(i + 1);
        right[i] = static_cast<float>((i + 1) * 10);
    }
    return Clip(std::move(left), std::move(right));
}

TimelinePart Placed(AudioClipPtr clip, int64_t startFrame, float left = 1.0f, float right = 1.0f) {
    TimelinePart part;
    part.clip = std::move(clip);
    part.startFrame = startFrame;
    part.leftGain = left;
    part.rightGain = right;
    return part;
}

/// A snapshot of parts, gains already applied, as the audio thread sees one.
TimelinePtr Snapshot(std::vector<TimelinePart> parts, double sampleRate = 44100.0) {
    auto timeline = std::make_shared<Timeline>();
    timeline->parts = std::move(parts);
    timeline->sampleRate = sampleRate;
    return timeline;
}

PartLayout Layout(int trackNo, double startMs, double endMs, const std::string &hash) {
    PartLayout part;
    part.trackNo = trackNo;
    part.startMs = startMs;
    part.endMs = endMs;
    part.audioHash = hash;
    return part;
}

TrackInfo Track(double volume, double pan, bool muted = false) {
    TrackInfo track;
    track.name = "T";
    track.volume = volume;
    track.pan = pan;
    track.muted = muted;
    return track;
}

/// Interleaved little-endian float32, the way §6.1 puts it on the wire, so a clip can be put
/// into a store the way one really arrives.
std::vector<uint8_t> Wire(const std::vector<float> &left, const std::vector<float> &right) {
    std::vector<uint8_t> bytes(left.size() * 2 * sizeof(float));
    for (size_t i = 0; i < left.size(); i++) {
        float pair[2] = {left[i], right[i]};
        std::memcpy(bytes.data() + i * 2 * sizeof(float), pair, sizeof(pair));
    }
    return bytes;
}

/// Two blocks of buffer, so a test can see what was written outside the part as well as in it.
struct Block {
    std::vector<float> left;
    std::vector<float> right;

    explicit Block(size_t frames, float fill = 0.0f)
        : left(frames, fill), right(frames, fill) {}

    void Mix(const Timeline &timeline, int64_t fromFrame) {
        bridge::MixTimeline(timeline, fromFrame, left.size(), left.data(), right.data());
    }
};

}  // namespace

TEST_CASE("A part lands at its own frame position and nowhere else") {
    TimelinePtr timeline = Snapshot({Placed(Ramp(4), 2)});
    Block block(8);
    block.Mix(*timeline, 0);

    CHECK(block.left == std::vector<float>{0, 0, 1, 2, 3, 4, 0, 0});
    CHECK(block.right == std::vector<float>{0, 0, 10, 20, 30, 40, 0, 0});
}

TEST_CASE("The mix adds, so the caller's buffer decides what silence is") {
    // The plugin clears its output itself; a mixer that assigned would erase whatever a part
    // placed earlier in the same block had already contributed.
    TimelinePtr timeline = Snapshot({Placed(Ramp(2), 1)});
    Block block(4, 0.5f);
    block.Mix(*timeline, 0);

    CHECK(block.left == std::vector<float>{0.5f, 1.5f, 2.5f, 0.5f});
}

TEST_CASE("Blocks that miss the part entirely leave the buffer untouched") {
    TimelinePtr timeline = Snapshot({Placed(Ramp(4), 100)});

    Block before(8);
    before.Mix(*timeline, 0);
    CHECK(before.left == std::vector<float>(8, 0.0f));

    Block after(8);
    after.Mix(*timeline, 104);  // The part ended on frame 103.
    CHECK(after.left == std::vector<float>(8, 0.0f));
}

TEST_CASE("A part is entered and left mid-block") {
    TimelinePtr timeline = Snapshot({Placed(Ramp(4), 10)});

    SUBCASE("the block ends inside the part") {
        Block block(4);
        block.Mix(*timeline, 8);
        CHECK(block.left == std::vector<float>{0, 0, 1, 2});
    }
    SUBCASE("the block starts inside the part") {
        Block block(4);
        block.Mix(*timeline, 12);
        CHECK(block.left == std::vector<float>{3, 4, 0, 0});
    }
    SUBCASE("the block is wholly inside the part") {
        Block block(2);
        block.Mix(*timeline, 11);
        CHECK(block.left == std::vector<float>{2, 3});
    }
}

TEST_CASE("A part that begins before the timeline contributes only its tail") {
    // Not reachable from a layout, whose positions are all positive, but the transport can be
    // moved to zero mid-part and the arithmetic has to hold either way.
    TimelinePtr timeline = Snapshot({Placed(Ramp(4), -2)});
    Block block(4);
    block.Mix(*timeline, 0);
    CHECK(block.left == std::vector<float>{3, 4, 0, 0});
}

TEST_CASE("Parts overlapping on one track sum, as they do in OpenUtau's own mixdown") {
    TimelinePtr timeline = Snapshot({Placed(Ramp(4), 0), Placed(Ramp(4), 2)});
    Block block(6);
    block.Mix(*timeline, 0);
    CHECK(block.left == std::vector<float>{1, 2, 3 + 1, 4 + 2, 3, 4});
}

TEST_CASE("Each channel gets its own gain in the generic mixer primitive") {
    // This low-level test keeps arbitrary gains useful for future host-side routing; BuildTimeline
    // policy below now supplies unity for the DAW-owned mixer.
    TimelinePtr timeline = Snapshot({Placed(Ramp(2), 0, 0.5f, 0.0f)});
    Block block(2);
    block.Mix(*timeline, 0);
    CHECK(block.left == std::vector<float>{0.5f, 1.0f});
    CHECK(block.right == std::vector<float>{0.0f, 0.0f});  // Panned hard left.
}

TEST_CASE("A zero-length block and an empty snapshot are both no-ops") {
    TimelinePtr timeline = Snapshot({Placed(Ramp(4), 0)});
    // A host is allowed to ask for no frames, and then the buffers may not even be there.
    bridge::MixTimeline(*timeline, 0, 0, nullptr, nullptr);

    Block block(4, 1.0f);
    block.Mix(*Snapshot({}), 0);
    CHECK(block.left == std::vector<float>(4, 1.0f));
}

TEST_CASE("A snapshot reports where its last part ends") {
    CHECK(Snapshot({})->Frames() == 0);
    CHECK(Snapshot({Placed(Ramp(4), 10), Placed(Ramp(4), 2)})->Frames() == 14);
    // Wholly before zero: there is nothing to play, and a negative length is not a length.
    CHECK(Snapshot({Placed(Ramp(4), -10)})->Frames() == 0);
}

TEST_CASE("Building a snapshot resolves position and track in one pass") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);  // The wire rate, so the samples stay exact.
    REQUIRE(store.Insert("mine", Wire({1.0f, 2.0f}, {3.0f, 4.0f})));
    REQUIRE(store.Insert("theirs", Wire({9.0f}, {9.0f})));

    std::vector<PartLayout> layout = {
        Layout(0, 500.0, 600.0, "theirs"),
        Layout(1, 1000.0, 1100.0, "mine"),
        Layout(1, 0.0, 100.0, "absent"),  // Not pulled yet.
    };
    std::vector<TrackInfo> tracks = {Track(0.0, 0.0), Track(0.0, -100.0)};

    TimelinePtr timeline = bridge::BuildTimeline(layout, tracks, store, 1, 44100.0);
    REQUIRE(static_cast<bool>(timeline));
    CHECK(timeline->sampleRate == 44100.0);
    // Only this track's parts, and only those whose audio is here.
    REQUIRE(timeline->parts.size() == 1);
    CHECK(timeline->parts[0].startFrame == 44100);  // 1000 ms.
    CHECK(timeline->parts[0].clip->left == std::vector<float>{1.0f, 2.0f});
    // Track mixing controls are intentionally not applied here: the DAW owns gain and pan.
    CHECK(timeline->parts[0].leftGain == doctest::Approx(1.0));
    CHECK(timeline->parts[0].rightGain == doctest::Approx(1.0));
}

TEST_CASE("A centred track remains unity because the DAW owns its mixer") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    REQUIRE(store.Insert("a", Wire({1.0f}, {2.0f})));
    std::vector<TrackInfo> tracks = {Track(-18.0, 100.0, true)};

    TimelinePtr timeline =
        bridge::BuildTimeline({Layout(0, 0.0, 100.0, "a")}, tracks, store, 0, 44100.0);
    REQUIRE(timeline->parts.size() == 1);
    CHECK(timeline->parts[0].leftGain == 1.0f);
    CHECK(timeline->parts[0].rightGain == 1.0f);

    Block block(1);
    block.Mix(*timeline, 0);
    CHECK(block.left == std::vector<float>{1.0f});
    CHECK(block.right == std::vector<float>{2.0f});
}

TEST_CASE("Track volume, pan and mute do not alter pre-fader audio") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    REQUIRE(store.Insert("a", Wire({1.0f}, {2.0f})));
    std::vector<PartLayout> layout = {Layout(0, 0.0, 100.0, "a")};

    for (TrackInfo track : {Track(0.0, 0.0), Track(-24.0, -100.0, true),
                            Track(12.0, 100.0, false)}) {
        TimelinePtr timeline = bridge::BuildTimeline(layout, {track}, store, 0, 44100.0);
        REQUIRE(timeline->parts.size() == 1);
        Block block(1);
        block.Mix(*timeline, 0);
        CHECK(block.left == std::vector<float>{1.0f});
        CHECK(block.right == std::vector<float>{2.0f});
    }
}
TEST_CASE("A millisecond position rounds to the nearest frame, not towards zero") {
    AudioStore store;
    store.SetHostSampleRate(48000.0);
    REQUIRE(store.Insert("a", Wire({1.0f}, {1.0f})));
    std::vector<TrackInfo> tracks = {Track(0.0, 0.0)};

    TimelinePtr timeline =
        bridge::BuildTimeline({Layout(0, 0.7, 2.0, "a")}, tracks, store, 0, 48000.0);
    REQUIRE(timeline->parts.size() == 1);
    CHECK(timeline->parts[0].startFrame == 34);  // 33.6 frames.
}

TEST_CASE("Missing audio is silent but track mixer controls are not applied") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    REQUIRE(store.Insert("a", Wire({1.0f}, {1.0f})));
    REQUIRE(store.Insert("empty", {}));
    std::vector<PartLayout> layout = {Layout(0, 0.0, 100.0, "a")};

    SUBCASE("a muted track still routes its dry part") {
        std::vector<TrackInfo> tracks = {Track(0.0, 0.0, true)};
        auto timeline = bridge::BuildTimeline(layout, tracks, store, 0, 44100.0);
        REQUIRE(timeline->parts.size() == 1);
        CHECK(timeline->parts[0].leftGain == 1.0f);
        CHECK(timeline->parts[0].rightGain == 1.0f);
    }
    SUBCASE("a track under the former fader floor still routes its dry part") {
        std::vector<TrackInfo> tracks = {Track(-24.0, 0.0)};
        CHECK(bridge::BuildTimeline(layout, tracks, store, 0, 44100.0)->parts.size() == 1);
    }
    SUBCASE("a track the project does not have") {
        std::vector<TrackInfo> tracks = {Track(0.0, 0.0)};
        CHECK(bridge::BuildTimeline(layout, tracks, store, 1, 44100.0)->parts.empty());
        CHECK(bridge::BuildTimeline(layout, tracks, store, -1, 44100.0)->parts.empty());
        // Before `updateTracks` arrives there is no track to route to.
        CHECK(bridge::BuildTimeline(layout, {}, store, 0, 44100.0)->parts.empty());
    }
    SUBCASE("no host rate yet") {
        std::vector<TrackInfo> tracks = {Track(0.0, 0.0)};
        TimelinePtr timeline = bridge::BuildTimeline(layout, tracks, store, 0, 0.0);
        CHECK(timeline->parts.empty());
        CHECK(timeline->sampleRate == 0.0);
    }
    SUBCASE("a part of no frames") {
        std::vector<TrackInfo> tracks = {Track(0.0, 0.0)};
        std::vector<PartLayout> empty = {Layout(0, 0.0, 0.0, "empty")};
        CHECK(bridge::BuildTimeline(empty, tracks, store, 0, 44100.0)->parts.empty());
    }
}

TEST_CASE("A snapshot keeps its audio alive after the store has dropped it") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    REQUIRE(store.Insert("a", Wire({1.0f, 2.0f}, {1.0f, 2.0f})));
    std::vector<TrackInfo> tracks = {Track(0.0, -100.0)};  // Mixer settings do not alter dry audio.
    TimelinePtr timeline =
        bridge::BuildTimeline({Layout(0, 0.0, 100.0, "a")}, tracks, store, 0, 44100.0);
    REQUIRE(timeline->parts.size() == 1);

    // The user deletes the part while a block is being rendered from it.
    store.Retain({});
    CHECK(store.Count() == 0);
    Block block(2);
    block.Mix(*timeline, 0);
    CHECK(block.left == std::vector<float>{1.0f, 2.0f});
}

TEST_CASE("Nothing is published until the worker publishes it") {
    TimelineBox box;
    TimelineBox::Borrow borrow(box);
    CHECK(!borrow);
    CHECK(borrow.Get() == nullptr);
    CHECK(box.RetiredCount() == 0);
    box.Collect();  // Nothing to free, and no reason to fail.
}

TEST_CASE("A published snapshot is what the next block sees") {
    TimelineBox box;
    TimelinePtr timeline = Snapshot({Placed(Ramp(4), 0)});
    box.Publish(timeline);

    TimelineBox::Borrow borrow(box);
    REQUIRE(borrow);
    CHECK(borrow.Get() == timeline.get());
    CHECK(borrow->parts.size() == 1);
}

TEST_CASE("A snapshot being read is not freed under it") {
    TimelineBox box;
    box.Publish(Snapshot({Placed(Ramp(4), 0)}));
    std::weak_ptr<const Timeline> first = box.Current();
    const Timeline *firstRaw = box.Current().get();

    {
        TimelineBox::Borrow borrow(box);
        REQUIRE(borrow.Get() == firstRaw);

        // A new layout arrives mid-block. The block keeps the snapshot it started with: a
        // buffer half filled from one snapshot and half from another would be a glitch.
        box.Publish(Snapshot({Placed(Ramp(8), 100)}));
        box.Collect();
        CHECK(borrow.Get() == firstRaw);
        CHECK(box.RetiredCount() == 1);
        CHECK(!first.expired());
    }

    // The block is over; the next Collect is where the memory actually goes back.
    box.Collect();
    CHECK(box.RetiredCount() == 0);
    CHECK(first.expired());

    TimelineBox::Borrow next(box);
    REQUIRE(next);
    CHECK(next->parts[0].startFrame == 100);
}

TEST_CASE("Snapshots published while nothing is reading are all reclaimed") {
    TimelineBox box;
    std::vector<std::weak_ptr<const Timeline>> published;
    for (int i = 0; i < 8; i++) {
        TimelinePtr timeline = Snapshot({Placed(Ramp(2), i)});
        published.push_back(timeline);
        box.Publish(std::move(timeline));
    }
    CHECK(box.RetiredCount() == 7);  // Every one but the live snapshot.

    box.Collect();
    CHECK(box.RetiredCount() == 0);
    for (size_t i = 0; i + 1 < published.size(); i++) {
        CHECK(published[i].expired());
    }
    CHECK(!published.back().expired());  // The live one stays, since it is what plays.
}

TEST_CASE("The handover holds up with a reader and a writer running at once") {
    // Not a proof — a race is not reliably reproducible — but a handshake that is wrong in the
    // obvious ways (freeing what is borrowed, or never freeing anything) fails here.
    constexpr int kRounds = 4000;
    TimelineBox box;
    box.Publish(Snapshot({Placed(Ramp(4), 0)}));

    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};
    std::atomic<int> bad{0};
    std::thread audio([&] {
        while (!stop.load()) {
            TimelineBox::Borrow borrow(box);
            if (!borrow) {
                bad.fetch_add(1);
                continue;
            }
            // What a real block does with a snapshot: read the placement, then the samples.
            Block block(8);
            block.Mix(*borrow.Get(), borrow->parts[0].startFrame);
            if (borrow->parts.size() != 1 || borrow->parts[0].clip->Frames() != 4 ||
                block.left[0] != 1.0f) {
                bad.fetch_add(1);
            }
            blocks.fetch_add(1);
        }
    });

    // The reader has to be inside its loop before the writer starts retiring. These rounds
    // are over in milliseconds, which is faster than a loaded runner sometimes starts a
    // thread — a reader that was never scheduled is not a concurrency test, it is a flake.
    bool readerRan = false;
    for (int i = 0; i < 5000 && !readerRan; i++) {
        readerRan = blocks.load() > 0 || bad.load() > 0;
        if (!readerRan) {
            std::this_thread::yield();
        }
    }
    CHECK(readerRan);

    for (int i = 1; i <= kRounds; i++) {
        box.Publish(Snapshot({Placed(Ramp(4), i)}));
        box.Collect();
        CHECK(box.RetiredCount() <= 1);  // At most the one the reader is on.
    }
    stop.store(true);
    audio.join();

    box.Collect();
    CHECK(box.RetiredCount() == 0);
    CHECK(bad.load() == 0);
    CHECK(blocks.load() > 0);
}
