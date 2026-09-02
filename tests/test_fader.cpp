#include <doctest.h>

#include "fader.h"

#include <cmath>

using bridge::DecibelToGain;
using bridge::PanToChannelGains;
using bridge::TrackGains;

TEST_CASE("unity is unity") {
    CHECK(DecibelToGain(0.0) == doctest::Approx(1.0f));
}

TEST_CASE("the curve is a plain decibel conversion above -16 dB") {
    CHECK(DecibelToGain(-6.0) == doctest::Approx(std::pow(10.0, -6.0 / 20.0)));
    CHECK(DecibelToGain(6.0) == doctest::Approx(std::pow(10.0, 6.0 / 20.0)));
    CHECK(DecibelToGain(-16.0) == doctest::Approx(std::pow(10.0, -16.0 / 20.0)));
}

TEST_CASE("the curve steepens below -16 dB") {
    // This is the whole reason the plugin cannot just use 10^(dB/20): at -20 dB OpenUtau is
    // a full 4 dB quieter than a naive conversion would be.
    CHECK(DecibelToGain(-20.0) == doctest::Approx(std::pow(10.0, -24.0 / 20.0)));
    CHECK(DecibelToGain(-20.0) < std::pow(10.0, -20.0 / 20.0));
}

TEST_CASE("the curve is continuous where it changes slope") {
    CHECK(DecibelToGain(-16.0) == doctest::Approx(DecibelToGain(-16.0 - 1e-9)));
}

TEST_CASE("-24 dB is a hard mute, not a small gain") {
    CHECK(DecibelToGain(-24.0) == 0.0f);
    CHECK(DecibelToGain(-24.5) == 0.0f);
    CHECK(DecibelToGain(-96.0) == 0.0f);
    // And it really is a jump: just above the threshold the gain is still audible.
    CHECK(DecibelToGain(-23.9) > 0.02f);
}

TEST_CASE("pan is constant power") {
    float left = 0.0f;
    float right = 0.0f;

    PanToChannelGains(0.0, &left, &right);
    CHECK(left == doctest::Approx(0.70710678f).epsilon(1e-5));
    CHECK(right == doctest::Approx(0.70710678f).epsilon(1e-5));
    CHECK(left * left + right * right == doctest::Approx(1.0f).epsilon(1e-5));

    PanToChannelGains(-100.0, &left, &right);
    CHECK(left == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(right == doctest::Approx(0.0f).epsilon(1e-5));

    PanToChannelGains(100.0, &left, &right);
    CHECK(left == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(right == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("pan is clamped rather than wrapped") {
    float left = 0.0f;
    float right = 0.0f;
    float clampedLeft = 0.0f;
    float clampedRight = 0.0f;
    PanToChannelGains(-1000.0, &left, &right);
    PanToChannelGains(-100.0, &clampedLeft, &clampedRight);
    CHECK(left == doctest::Approx(clampedLeft));
    CHECK(right == doctest::Approx(clampedRight));
}

TEST_CASE("a muted track contributes nothing whatever its fader says") {
    float left = 1.0f;
    float right = 1.0f;
    TrackGains(6.0, -50.0, true, &left, &right);
    CHECK(left == 0.0f);
    CHECK(right == 0.0f);
}

TEST_CASE("track gain is the fader and the pan together") {
    float left = 0.0f;
    float right = 0.0f;
    TrackGains(-6.0, 0.0, false, &left, &right);
    float expected = static_cast<float>(std::pow(10.0, -6.0 / 20.0) * 0.70710678);
    CHECK(left == doctest::Approx(expected).epsilon(1e-5));
    CHECK(right == doctest::Approx(expected).epsilon(1e-5));
}
