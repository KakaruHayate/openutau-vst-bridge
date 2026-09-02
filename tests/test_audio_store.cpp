#include "audio_store.h"

#include <doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using bridge::AudioClipPtr;
using bridge::AudioStore;
using bridge::PartLayout;

namespace {

/// Interleaved little-endian float32, the way §6.1 puts it on the wire.
std::vector<uint8_t> Wire(const std::vector<float> &left, const std::vector<float> &right) {
    REQUIRE(left.size() == right.size());
    std::vector<uint8_t> bytes(left.size() * 2 * sizeof(float));
    for (size_t i = 0; i < left.size(); i++) {
        float pair[2] = {left[i], right[i]};
        std::memcpy(bytes.data() + i * 2 * sizeof(float), pair, sizeof(pair));
    }
    return bytes;
}

std::vector<float> Sine(size_t frames, double frequency, double rate, float amplitude = 1.0f) {
    std::vector<float> samples(frames);
    for (size_t i = 0; i < frames; i++) {
        samples[i] = static_cast<float>(
            amplitude * std::sin(2.0 * 3.14159265358979323846 * frequency *
                                 static_cast<double>(i) / rate));
    }
    return samples;
}

/// Over the middle of the signal only: a resampler's filter needs a few hundred samples at each
/// end to settle, and that ringing is not what these tests are about.
double MiddleRms(const std::vector<float> &samples) {
    if (samples.size() < 10) {
        return 0.0;
    }
    size_t from = samples.size() / 10;
    size_t to = samples.size() - samples.size() / 10;
    double sum = 0.0;
    for (size_t i = from; i < to; i++) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

PartLayout Part(const std::string &hash) {
    PartLayout part;
    part.audioHash = hash;
    return part;
}

}  // namespace

TEST_CASE("Nothing is stored before the host reports its rate") {
    AudioStore store;
    CHECK(store.HostSampleRate() == 0.0);
    CHECK(!store.Insert("1", Wire({0.5f}, {-0.5f})));
    CHECK(store.Count() == 0);
}

TEST_CASE("At the wire rate a clip is stored sample for sample") {
    // 44.1 kHz is the common case and the one OpenUtau renders at; converting there would only
    // colour audio that needs no conversion at all.
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    std::vector<float> left = {0.0f, 0.25f, -0.5f, 1.0f, -1.0f};
    std::vector<float> right = {1.0f, -0.25f, 0.5f, -1.0f, 0.0f};
    REQUIRE(store.Insert("1", Wire(left, right)));

    AudioClipPtr clip = store.Find("1");
    REQUIRE(static_cast<bool>(clip));
    CHECK(clip->Frames() == 5);
    CHECK(clip->left == left);
    CHECK(clip->right == right);  // Channels split, not swapped or summed.
}

TEST_CASE("A ragged payload is refused") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    std::vector<uint8_t> ragged(6, 0);
    CHECK(!store.Insert("1", ragged));
    CHECK(store.Count() == 0);
}

TEST_CASE("An empty part is a valid clip of no frames") {
    AudioStore store;
    store.SetHostSampleRate(48000.0);
    REQUIRE(store.Insert("1", {}));
    AudioClipPtr clip = store.Find("1");
    REQUIRE(static_cast<bool>(clip));
    CHECK(clip->Frames() == 0);
}

TEST_CASE("A clip is converted to the host rate, keeping its length and level") {
    AudioStore store;
    store.SetHostSampleRate(48000.0);
    std::vector<float> tone = Sine(4410, 1000.0, 44100.0);
    REQUIRE(store.Insert("1", Wire(tone, tone)));

    AudioClipPtr clip = store.Find("1");
    REQUIRE(static_cast<bool>(clip));
    CHECK(clip->Frames() == 4800);  // ceil(4410 * 48000 / 44100)
    CHECK(clip->left.size() == clip->right.size());
    // A full-scale sine is 0.7071 RMS; a resampler that dropped, doubled or attenuated frames
    // would not land there.
    CHECK(MiddleRms(clip->left) == doctest::Approx(0.7071).epsilon(0.02));
    CHECK(MiddleRms(clip->right) == doctest::Approx(0.7071).epsilon(0.02));
}

TEST_CASE("Conversion downwards works the same way") {
    AudioStore store;
    store.SetHostSampleRate(32000.0);
    std::vector<float> tone = Sine(8820, 440.0, 44100.0, 0.5f);
    REQUIRE(store.Insert("1", Wire(tone, tone)));

    AudioClipPtr clip = store.Find("1");
    REQUIRE(static_cast<bool>(clip));
    CHECK(clip->Frames() == 6400);  // ceil(8820 * 32000 / 44100)
    CHECK(MiddleRms(clip->left) == doctest::Approx(0.3536).epsilon(0.02));
}

TEST_CASE("Silence stays silent through conversion") {
    AudioStore store;
    store.SetHostSampleRate(96000.0);
    std::vector<float> silence(1000, 0.0f);
    REQUIRE(store.Insert("1", Wire(silence, silence)));

    AudioClipPtr clip = store.Find("1");
    REQUIRE(static_cast<bool>(clip));
    for (float sample : clip->left) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("Missing names exactly what has to be pulled") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    REQUIRE(store.Insert("held", Wire({0.0f}, {0.0f})));

    std::vector<PartLayout> parts = {Part("held"), Part("a"), Part("b"), Part("a"), Part("")};
    std::vector<std::string> missing = store.Missing(parts);
    // Held hashes, repeats and empty hashes all drop out; order follows the layout.
    REQUIRE(missing.size() == 2);
    CHECK(missing[0] == "a");
    CHECK(missing[1] == "b");
    CHECK(store.Missing({}).empty());
}

TEST_CASE("A host rate change drops the clips, since every one of them is at the old rate") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    REQUIRE(store.Insert("1", Wire({0.5f}, {0.5f})));
    CHECK(store.Count() == 1);

    store.SetHostSampleRate(48000.0);
    CHECK(store.Count() == 0);
    CHECK(store.HostSampleRate() == 48000.0);
    // The same rate again is not a change, so it costs nothing.
    REQUIRE(store.Insert("1", Wire({0.5f}, {0.5f})));
    store.SetHostSampleRate(48000.0);
    CHECK(store.Count() == 1);
    // A host that reports nonsense is ignored rather than obeyed.
    store.SetHostSampleRate(0.0);
    CHECK(store.HostSampleRate() == 48000.0);
    CHECK(store.Count() == 1);
}

TEST_CASE("Retain frees what the layout no longer names") {
    AudioStore store;
    store.SetHostSampleRate(44100.0);
    REQUIRE(store.Insert("keep", Wire({0.5f}, {0.5f})));
    REQUIRE(store.Insert("drop", Wire({0.5f}, {0.5f})));
    CHECK(store.Bytes() == 4 * sizeof(float));

    // A clip handed out stays alive for as long as its holder needs it, which is what lets an
    // audio-thread snapshot outlive the layout that named it.
    AudioClipPtr borrowed = store.Find("drop");
    store.Retain({"keep"});
    CHECK(store.Count() == 1);
    CHECK(store.Holds("keep"));
    CHECK(!store.Holds("drop"));
    CHECK(!store.Find("drop"));
    REQUIRE(static_cast<bool>(borrowed));
    CHECK(borrowed->Frames() == 1);

    store.Clear();
    CHECK(store.Count() == 0);
    CHECK(store.Bytes() == 0);
}
