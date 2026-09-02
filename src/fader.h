#pragma once

/*
 * OpenUtau's fader, reproduced (PROTOCOL.md §6.1). Part audio arrives pre-fader, so the
 * plugin has to apply this itself, and a plain 10^(dB/20) is not it: the curve steepens
 * below -16 dB and hard-mutes at -24.
 *
 * Header-only and allocation-free — the audio thread calls it (see timeline.h).
 */

#include <algorithm>
#include <cmath>

namespace bridge {

/// `PlaybackManager.DecibelToVolume`.
inline float DecibelToGain(double decibels) {
    if (decibels <= -24.0) {
        return 0.0f;
    }
    double shaped = decibels < -16.0 ? decibels * 2.0 + 16.0 : decibels;
    return static_cast<float>(std::pow(10.0, shaped / 20.0));
}

/// `MusicMath.PanToChannelVolumes`. Computed in float, as it is there, so the two sides
/// agree to the last bit rather than merely to the ear.
inline void PanToChannelGains(double pan, float *left, float *right) {
    constexpr float kHalfPi = static_cast<float>(3.14159265358979323846 / 2.0);
    float clamped = std::clamp(static_cast<float>(pan), -100.0f, 100.0f);
    float angle = (clamped + 100.0f) / 200.0f * kHalfPi;
    *left = std::cos(angle);
    *right = std::sin(angle);
}

/// The whole per-track gain, mirroring `RenderEngine`: a muted track contributes nothing.
inline void TrackGains(double volume, double pan, bool muted, float *left, float *right) {
    if (muted) {
        *left = 0.0f;
        *right = 0.0f;
        return;
    }
    float gain = DecibelToGain(volume);
    PanToChannelGains(pan, left, right);
    *left *= gain;
    *right *= gain;
}

}  // namespace bridge
