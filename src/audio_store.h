#pragma once

/*
 * The hash-keyed audio the mixer draws from. Everything OpenUtau sends is at 44.1 kHz
 * (PROTOCOL.md §6.1); the host asks for its own rate, so a clip is converted once on arrival
 * rather than per block.
 *
 * Worker thread only. The audio thread never touches the store, only the AudioClipPtrs a
 * snapshot handed it, which is why a clip is immutable once built.
 */

#include "messages.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {

/// One part's rendered audio at the host's sample rate, deinterleaved because that is how both
/// the resampler and every plugin format want it.
struct AudioClip {
    std::vector<float> left;
    std::vector<float> right;

    size_t Frames() const { return left.size(); }
    size_t Bytes() const { return (left.size() + right.size()) * sizeof(float); }
};

using AudioClipPtr = std::shared_ptr<const AudioClip>;

class AudioStore {
public:
    /// The rate everything on the wire is at (§6.1). Not negotiable: OpenUtau's render pipeline
    /// is fixed at it.
    static constexpr double kWireRate = 44100.0;

    /// Clips are converted on arrival, so a rate change invalidates all of them. They are
    /// dropped here; re-pulling them is the session's job, and the hashes are still in the last
    /// layout, so `getAudio` alone is enough — no message exists, or is needed, to ask OpenUtau
    /// to re-send a layout.
    void SetHostSampleRate(double rate);

    /// 0 until the host has reported one, which is when Insert starts working.
    double HostSampleRate() const { return hostRate_; }

    bool Holds(const std::string &hash) const;

    /// The hashes in `parts` that are not held, de-duplicated and in first-seen order: exactly
    /// what an `updatePartLayout` answer owes (§6.2).
    std::vector<std::string> Missing(const std::vector<PartLayout> &parts) const;

    /// Decodes wire bytes — 44.1 kHz stereo interleaved little-endian float32 — and converts
    /// them to the host rate. False if the payload is not whole stereo frames, or if no host
    /// rate is known yet.
    bool Insert(const std::string &hash, const std::vector<uint8_t> &pcm);

    /// Null when not held. The pointer keeps the clip alive even if the store drops it, which is
    /// what lets a snapshot outlive a layout change.
    AudioClipPtr Find(const std::string &hash) const;

    /// Drops every clip not listed, so parts the user deleted stop costing memory.
    void Retain(const std::vector<std::string> &hashes);

    void Clear();

    size_t Count() const { return clips_.size(); }
    size_t Bytes() const;

private:
    double hostRate_ = 0.0;
    std::unordered_map<std::string, AudioClipPtr> clips_;
};

}  // namespace bridge
