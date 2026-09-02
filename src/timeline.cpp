#include "timeline.h"

#include "fader.h"

#include <algorithm>
#include <cmath>

namespace bridge {

size_t Timeline::Frames() const {
    int64_t end = 0;
    for (const TimelinePart &part : parts) {
        end = std::max(end, part.startFrame + static_cast<int64_t>(part.clip->Frames()));
    }
    return end <= 0 ? 0u : static_cast<size_t>(end);
}

TimelinePtr BuildTimeline(const std::vector<PartLayout> &parts,
                          const std::vector<TrackInfo> &tracks, const AudioStore &store,
                          int trackNo, double sampleRate) {
    auto timeline = std::make_shared<Timeline>();
    timeline->sampleRate = sampleRate;
    if (sampleRate <= 0.0 || trackNo < 0 || static_cast<size_t>(trackNo) >= tracks.size()) {
        // Silence, not unity gain: an instance whose track the project does not have — because
        // it was deleted, or because `updateTracks` has not arrived yet — has no volume to play
        // at, and guessing one would mean a burst at the wrong level for the fraction of a
        // second before the real value lands.
        return timeline;
    }
    const TrackInfo &track = tracks[static_cast<size_t>(trackNo)];
    float leftGain = 0.0f;
    float rightGain = 0.0f;
    TrackGains(track.volume, track.pan, track.muted, &leftGain, &rightGain);
    if (leftGain == 0.0f && rightGain == 0.0f) {
        // Muted, or below the fader's -24 dB floor. Keeping the parts would cost a snapshot
        // full of clips that multiply out to nothing.
        return timeline;
    }
    for (const PartLayout &part : parts) {
        if (part.trackNo != trackNo) {
            continue;
        }
        AudioClipPtr clip = store.Find(part.audioHash);
        if (!clip || clip->Frames() == 0) {
            // Not pulled yet, or an empty part. Either way there is nothing to place; the pull
            // that fills it will be followed by a new snapshot.
            continue;
        }
        TimelinePart placed;
        placed.startFrame = static_cast<int64_t>(std::llround(part.startMs / 1000.0 * sampleRate));
        placed.clip = std::move(clip);
        placed.leftGain = leftGain;
        placed.rightGain = rightGain;
        timeline->parts.push_back(std::move(placed));
    }
    return timeline;
}

void MixTimeline(const Timeline &timeline, int64_t fromFrame, size_t frames, float *left,
                 float *right) {
    if (frames == 0) {
        return;
    }
    const int64_t blockEnd = fromFrame + static_cast<int64_t>(frames);
    for (const TimelinePart &part : timeline.parts) {
        const int64_t partEnd = part.startFrame + static_cast<int64_t>(part.clip->Frames());
        const int64_t begin = std::max(fromFrame, part.startFrame);
        const int64_t end = std::min(blockEnd, partEnd);
        if (end <= begin) {
            continue;
        }
        // The part's own length decides where it stops, not the layout's window: the window is
        // OpenUtau's part boundary rounded to a millisecond, and trimming to it would cut the
        // release tail that the render deliberately includes.
        const size_t count = static_cast<size_t>(end - begin);
        const size_t out = static_cast<size_t>(begin - fromFrame);
        const size_t in = static_cast<size_t>(begin - part.startFrame);
        const float *clipLeft = part.clip->left.data() + in;
        const float *clipRight = part.clip->right.data() + in;
        for (size_t i = 0; i < count; i++) {
            left[out + i] += clipLeft[i] * part.leftGain;
            right[out + i] += clipRight[i] * part.rightGain;
        }
    }
}

/*
 * The four atomic operations below are left at the default seq_cst on purpose. Release/acquire
 * would order each variable's own history, but this handshake needs the reader's *store* to
 * `borrowed_` to be visible before its *load* of `published_` — an ordering between two
 * different atomics that only a full fence provides. On x86 that is one locked instruction per
 * block, which is nothing next to the block itself.
 */

void TimelineBox::Publish(TimelinePtr next) {
    const Timeline *raw = next.get();
    if (live_) {
        // Retired before the swap, never freed here: freeing is what the audio thread must not
        // be made to do or wait on, so it happens in Collect() on this thread.
        retired_.push_back(std::move(live_));
    }
    live_ = std::move(next);
    published_.store(raw);
}

void TimelineBox::Collect() {
    const Timeline *inUse = borrowed_.load();
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [inUse](const TimelinePtr &snapshot) {
                                      return snapshot.get() != inUse;
                                  }),
                   retired_.end());
}

TimelineBox::Borrow::Borrow(TimelineBox &box) : box_(box) {
    const Timeline *seen = box_.published_.load();
    while (true) {
        box_.borrowed_.store(seen);
        const Timeline *again = box_.published_.load();
        if (again == seen) {
            break;
        }
        // It was replaced while we were announcing it, so Collect() may have decided it was
        // free before it could see the announcement. Take the replacement instead.
        seen = again;
    }
    timeline_ = seen;
}

TimelineBox::Borrow::~Borrow() {
    box_.borrowed_.store(nullptr);
}

}  // namespace bridge
