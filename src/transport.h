#pragma once

/*
 * Where a block sits on the host's timeline. CLAP reports the position in its own fixed-point
 * units, a part's window is in absolute milliseconds (PROTOCOL.md §6.1), and the mixer is indexed
 * in frames — this is the one place those three meet.
 *
 * Header-only and allocation-free: the audio thread calls it once per block.
 */

#include <clap/clap.h>

#include <cmath>
#include <cstdint>

namespace bridge {

inline bool IsPlaying(const clap_event_transport_t *transport) {
    return transport != nullptr && (transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
}

/// Seconds from the start of the host's timeline. False when the host reports no position, and
/// then there is no honest place to put the audio.
inline bool BlockSeconds(const clap_event_transport_t *transport, double *seconds) {
    if (transport == nullptr) {
        return false;
    }
    if ((transport->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0) {
        *seconds = static_cast<double>(transport->song_pos_seconds) / CLAP_SECTIME_FACTOR;
        return true;
    }
    // A host with only a musical timeline still gives a position, and seconds are what a part's
    // window is in. OpenUtau's own tempo is deliberately not consulted: the two projects are
    // aligned in time, not in bars, until tempo sync lands.
    if ((transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0 &&
        (transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 && transport->tempo > 0.0) {
        double beats = static_cast<double>(transport->song_pos_beats) / CLAP_BEATTIME_FACTOR;
        *seconds = beats * 60.0 / transport->tempo;
        return true;
    }
    return false;
}

/// The same position in frames at the host's rate, which is what the timeline is indexed by.
inline bool BlockStartFrame(const clap_event_transport_t *transport, double sampleRate,
                            int64_t *frame) {
    double seconds = 0.0;
    if (sampleRate <= 0.0 || !BlockSeconds(transport, &seconds)) {
        return false;
    }
    *frame = static_cast<int64_t>(std::llround(seconds * sampleRate));
    return true;
}

}  // namespace bridge
