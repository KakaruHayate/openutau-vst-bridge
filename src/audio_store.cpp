#include "audio_store.h"

#include "hash.h"
#include "log.h"

#include <CDSPResampler.h>

#include <bit>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace bridge {
namespace {

static_assert(std::endian::native == std::endian::little,
              "The wire format is little-endian float32 (PROTOCOL.md §6.1); a big-endian port "
              "has to byte-swap here rather than memcpy.");

constexpr size_t kBytesPerFrame = sizeof(float) * static_cast<size_t>(kWireChannels);

/// Interleaved wire bytes to two channels, no conversion beyond the split.
void Deinterleave(const std::vector<uint8_t> &pcm, std::vector<float> *left,
                  std::vector<float> *right) {
    size_t frames = pcm.size() / kBytesPerFrame;
    left->resize(frames);
    right->resize(frames);
    for (size_t i = 0; i < frames; i++) {
        float pair[2];
        std::memcpy(pair, pcm.data() + i * kBytesPerFrame, kBytesPerFrame);
        (*left)[i] = pair[0];
        (*right)[i] = pair[1];
    }
}

/// r8brain compensates whole-sample latency internally, so the output starts where the input
/// does and a part stays where the layout put it.
void Resample(double from, double to, std::vector<float> *channel, size_t outFrames) {
    if (channel->empty() || outFrames == 0) {
        channel->assign(outFrames, 0.0f);
        return;
    }
    // The block size, not the clip length: r8brain sizes its internal buffers from this, and a
    // three-minute part would otherwise ask for hundreds of megabytes of scratch. oneshot()
    // feeds the input a block at a time by itself.
    constexpr int kBlockFrames = 4096;
    int block = static_cast<int>(std::min<size_t>(channel->size(), kBlockFrames));
    std::vector<float> converted(outFrames);
    r8b::CDSPResampler24 resampler(from, to, block);
    resampler.oneshot(channel->data(), static_cast<int>(channel->size()), converted.data(),
                      static_cast<int>(outFrames));
    *channel = std::move(converted);
}

}  // namespace

void AudioStore::SetHostSampleRate(double rate) {
    if (rate <= 0.0 || rate == hostRate_) {
        return;
    }
    if (!clips_.empty()) {
        BRIDGE_INFO("Host sample rate is now %.0f Hz; dropping %zu converted clip(s).", rate,
                    clips_.size());
        clips_.clear();
    }
    hostRate_ = rate;
}

bool AudioStore::Holds(const std::string &hash) const {
    return clips_.find(hash) != clips_.end();
}

std::vector<std::string> AudioStore::Missing(const std::vector<PartLayout> &parts) const {
    std::vector<std::string> missing;
    std::unordered_set<std::string> seen;
    for (const PartLayout &part : parts) {
        if (part.audioHash.empty() || Holds(part.audioHash)) {
            continue;
        }
        // Two parts can share a hash — identical audio is stored and pulled once (§6.2).
        if (seen.insert(part.audioHash).second) {
            missing.push_back(part.audioHash);
        }
    }
    return missing;
}

bool AudioStore::Insert(const std::string &hash, const std::vector<uint8_t> &pcm) {
    if (hostRate_ <= 0.0) {
        BRIDGE_WARN("Dropping audio for %s: the host has not reported a sample rate yet.",
                    hash.c_str());
        return false;
    }
    if (pcm.size() % kBytesPerFrame != 0) {
        BRIDGE_WARN("Dropping audio for %s: %zu bytes is not whole stereo float32 frames.",
                    hash.c_str(), pcm.size());
        return false;
    }
    auto clip = std::make_shared<AudioClip>();
    Deinterleave(pcm, &clip->left, &clip->right);
    if (hostRate_ != kWireRate) {
        // Round up: a fractional last frame still has to be somewhere to go.
        size_t outFrames = static_cast<size_t>(
            std::ceil(static_cast<double>(clip->left.size()) * hostRate_ / kWireRate));
        Resample(kWireRate, hostRate_, &clip->left, outFrames);
        Resample(kWireRate, hostRate_, &clip->right, outFrames);
    }
    clips_[hash] = std::move(clip);
    return true;
}

AudioClipPtr AudioStore::Find(const std::string &hash) const {
    auto found = clips_.find(hash);
    return found == clips_.end() ? nullptr : found->second;
}

void AudioStore::Retain(const std::vector<std::string> &hashes) {
    std::unordered_set<std::string> keep(hashes.begin(), hashes.end());
    for (auto it = clips_.begin(); it != clips_.end();) {
        it = keep.count(it->first) != 0 ? std::next(it) : clips_.erase(it);
    }
}

void AudioStore::Clear() {
    clips_.clear();
}

size_t AudioStore::Bytes() const {
    size_t total = 0;
    for (const auto &entry : clips_) {
        total += entry.second->Bytes();
    }
    return total;
}

}  // namespace bridge
