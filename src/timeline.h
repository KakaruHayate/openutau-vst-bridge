#pragma once

/*
 * What the audio thread plays: parts placed at host-rate frame positions with their gains
 * already applied to them, and nothing else. Every decision that needs the layout, the track
 * list, the store or the fader is made once on the worker thread while building a snapshot,
 * so a block of audio costs one pointer load, a window intersection per part and a multiply-add.
 *
 * A snapshot is immutable once published. That is what makes the handover safe: the worker
 * never edits what the audio thread can see, it publishes a replacement and frees the old one
 * later, from its own thread (see TimelineBox).
 */

#include "audio_store.h"
#include "messages.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bridge {

/// One part, resolved: where it starts in host frames, and the gain each channel gets.
struct TimelinePart {
    /// Keeps the audio alive for as long as the snapshot is reachable, which is longer than
    /// the store holds it — a layout change must not pull audio out from under a running block.
    AudioClipPtr clip;

    /// Frames from the start of the OpenUtau timeline. Signed because the transport can be
    /// asked about a position before a part and the arithmetic has to stay honest there.
    int64_t startFrame = 0;

    float leftGain = 1.0f;
    float rightGain = 1.0f;
};

/// Everything one instance needs to render, for one moment in the project's life.
struct Timeline {
    /// This instance's track only: filtering per block would repeat a decision that cannot
    /// change without a new snapshot anyway.
    std::vector<TimelinePart> parts;

    /// The host rate the parts' frame positions are in. Kept so a stale snapshot can be
    /// recognised after the host changes rate, rather than played at the wrong speed.
    double sampleRate = 0.0;

    size_t Frames() const;
};

using TimelinePtr = std::shared_ptr<const Timeline>;

/// Builds the snapshot for `trackNo`. Parts on other tracks, parts whose audio has not arrived
/// and parts of a muted or fully attenuated track are all left out: the audio thread should
/// never spend a block discovering that something is inaudible.
TimelinePtr BuildTimeline(const std::vector<PartLayout> &parts,
                          const std::vector<TrackInfo> &tracks, const AudioStore &store,
                          int trackNo, double sampleRate);

/// Adds every part overlapping [fromFrame, fromFrame + frames) into the buffers. Adds rather
/// than assigns — the caller clears, and overlapping parts on one track sum, as they do in
/// OpenUtau's own mixdown. Allocation-free and lock-free: safe on the audio thread.
void MixTimeline(const Timeline &timeline, int64_t fromFrame, size_t frames, float *left,
                 float *right);

/*
 * The handover. `std::atomic<std::shared_ptr<T>>` would be the obvious answer, but MSVC
 * implements it with a spin lock the worker can be holding, so the audio thread would be made
 * to wait on a lower-priority thread. This is the single-reader hazard pointer instead: the
 * reader announces the snapshot it is reading, and the worker frees a retired snapshot only
 * once the reader is demonstrably somewhere else.
 */
class TimelineBox {
public:
    /// Worker: hand over `next`. Nothing is freed here — freeing is the audio thread's problem
    /// to avoid, so it happens in Collect().
    void Publish(TimelinePtr next);

    /// Worker: free the retired snapshots the reader has finished with. Cheap to call every
    /// poll; a snapshot the reader is still on stays for the next one.
    void Collect();

    /// Worker: what is published, for the worker's own use. Never call from the audio thread.
    TimelinePtr Current() const { return live_; }

    /// The audio thread's view of one snapshot, for exactly one block. Borrowing is a pair of
    /// atomic stores; the reader must not hold one across blocks, or nothing is ever freed.
    class Borrow {
    public:
        explicit Borrow(TimelineBox &box);
        ~Borrow();

        Borrow(const Borrow &) = delete;
        Borrow &operator=(const Borrow &) = delete;

        /// Null when nothing has been published yet, which is the normal state of a plugin
        /// whose OpenUtau has not connected.
        const Timeline *Get() const { return timeline_; }
        explicit operator bool() const { return timeline_ != nullptr; }
        const Timeline *operator->() const { return timeline_; }

    private:
        TimelineBox &box_;
        const Timeline *timeline_ = nullptr;
    };

    /// How many snapshots are waiting to be freed. For tests: it should not grow.
    size_t RetiredCount() const { return retired_.size(); }

private:
    /// Read by the audio thread, written by the worker.
    std::atomic<const Timeline *> published_{nullptr};
    /// Written by the audio thread, read by the worker: the snapshot in use, if any.
    std::atomic<const Timeline *> borrowed_{nullptr};

    /// Worker-owned ownership, mirroring `published_`, and what it displaced.
    TimelinePtr live_;
    std::vector<TimelinePtr> retired_;
};

}  // namespace bridge
