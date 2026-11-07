#pragma once

/*
 * One plugin instance's whole bridge: the listener, its advertisement, the connection when
 * there is one, the audio it has collected and the snapshot the audio thread plays.
 *
 * Thread rules, which are the reason this class exists at all:
 *   - The worker thread owns the connection, the store and the layout. Nothing else touches
 *     them, so none of them needs a lock.
 *   - The audio thread calls Render() and NoteTransport(). Both are wait-free.
 *   - Everything the host tells us on the main thread (sample rate, track index) arrives as an
 *     atomic the worker picks up on its next round, rather than as a call into its state.
 *
 * A disconnect is deliberately not a reset: the audio OpenUtau already rendered stays, so the
 * DAW can go on playing and mixing a project whose editor has been closed. Only a new layout
 * replaces the old one, and it does so wholesale.
 */

#include "audio_store.h"
#include "connection.h"
#include "discovery.h"
#include "socket.h"
#include "timeline.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bridge {

/// The highest track index the protocol and the plugin's parameter can express. The
/// picker clamps against this too: OpenUtau may report more tracks than fit, and a
/// request beyond it would report a parameter value outside the declared 0-63 range.
constexpr int kMaxTrackNo = 63;

/// What an info window shows about one instance. Copied out wholesale by `UiCopy()` on the
/// main thread; the numbers come from atomics, the strings from a mutex-guarded block.
struct UiState {
    bool connected = false;
    int port = 0;
    std::string projectName;
    bool projectSaved = false;
    /// v1.2: every track with its name and its singer/engine informational fields; the track
    /// picker renders "N: name - singer / engine" out of these.
    std::vector<TrackInfo> tracks;
    int trackNo = 0;
    bool hasTempo = false;
    double tempo = 0.0;
    bool playing = false;
};

class Session final : private ConnectionHandler {
public:
    /// Long enough that an idle instance costs nothing, short enough that Stop() is not felt as
    /// a hang when a host unloads a plugin.
    static constexpr int kPollMs = 50;

    /// A session that has not started: nothing bound, no worker, nothing advertised.
    Session() = default;

    /// The discovery directory is injectable for the same reason Discovery's is: a test that
    /// advertised itself in the real one would be found, and connected to, by a running OpenUtau.
    explicit Session(std::filesystem::path discoveryDirectory)
        : discovery_(std::move(discoveryDirectory)) {}

    /// Stops the worker and withdraws the advertisement; safe on a session that never started.
    ~Session() override;

    /// One listener, one worker, one connection: a copy would own a second of each, and the audio
    /// held is reachable from the DAW's thread, so there is nothing copyable here.
    Session(const Session &) = delete;
    /// See above: the audio a session holds is reached from the DAW's thread, so it is not copied.
    Session &operator=(const Session &) = delete;

    /// Main thread. Binds a port, advertises it, then starts the worker — in that order,
    /// because OpenUtau reads an advertisement as a promise that the port is already listening
    /// (§4). False if the port could not be bound or advertised, and then nothing is running.
    bool Start();

    /// Main thread. Idempotent, and safe to call on a session that never started.
    void Stop();

    /// True while the advertisement is up and the worker is running, i.e. between Start()
    /// returning true and Stop().
    bool IsRunning() const { return running_.load(); }

    /// 0 until Start() succeeds.
    int Port() const { return listener_.Port(); }

    /// True while a peer holds the socket. Audio and playback outlive this (see Render), so an
    /// instance whose OpenUtau quit keeps playing what it already holds.
    bool IsConnected() const { return connected_.load(); }

    /// The host's rate, from `activate`. Clips are converted on arrival, so a change drops
    /// every one of them and the worker re-pulls what the layout still names.
    void SetHostSampleRate(double rate) { hostRate_.store(rate); }

    /// Which OpenUtau track this instance routes. OpenUtau's volume/pan/mute fields remain on the
    /// wire for compatibility, but the bridge emits pre-fader audio and the DAW owns those controls.
    void SetTrackNo(int trackNo) { trackNo_.store(trackNo); }
    /// The track this instance is routing now, which the timeline was last built from.
    int TrackNo() const { return trackNo_.load(); }

    /// Main thread, from the track picker. Changes the routing immediately - the audio follows
    /// on the next block - and records a pending request so the plugin can report the new value
    /// to the host as a parameter change (CLAP: gesture + value + gesture on the output queue).
    /// The pending flag is what keeps a host-side automation and a GUI click from echoing.
    void RequestTrackNo(int trackNo) {
        trackNo_.store(std::clamp(trackNo, 0, kMaxTrackNo));
        trackRequestPending_.store(true);
    }

    /// Main thread, from the plugin's process/flush path. True exactly once per GUI change;
    /// the plugin answers it by notifying the host for the parameter identified below.
    bool ConsumeTrackRequest() { return trackRequestPending_.exchange(false); }

    /// Peek at the pending flag without taking it: the caller may still be refused by the
    /// host's output queue, and then the request must stay armed for the next visit.
    bool HasTrackRequest() const { return trackRequestPending_.load(); }

    /// Drop a pending request without reporting it - the host picked a track itself and
    /// its choice supersedes whatever the window had picked.
    void ClearTrackRequest() { trackRequestPending_.store(false); }

    /// Main thread. Whether the host is rendering faster than real time — a bounce, a freeze, an
    /// export. Audio arrives over a socket while the timeline is being played, which is fine at
    /// real-time speed and not fine at a bounce's, so offline Render() is allowed to wait for
    /// what has not arrived. Cleared again when the host goes back to real time.
    void SetOffline(bool offline) {
        offline_.store(offline);
        hopeless_.store(false);  // A new pass deserves a fresh chance to wait.
    }

    /// Audio thread. Offline blocks are filled even when the host does not report itself playing.
    bool IsOffline() const { return offline_.load(); }

    /// True when every part the layout names has its audio, converted and placed. This is what a
    /// bounce waits for, and the honest answer to "is what I would render now complete".
    bool IsSynced() const { return synced_.load(); }

    /// Audio thread. Fills the buffers with what the timeline holds over
    /// [fromFrame, fromFrame + frames) — assigns rather than adds, since this is the only
    /// source of the plugin's output. Silence when nothing is connected or nothing is placed.
    void Render(int64_t fromFrame, size_t frames, float *left, float *right);

    /// Audio thread. The transport, every block: position, play state and tempo. These feed
    /// the `playhead` and `bpm` notifications (v1.1) and the info window; the notifications
    /// themselves are sent from the worker, throttled there. The play state's rising edge is
    /// also what `playbackStarted` reports (§6.1).
    void NoteTransport(bool hasPosition, double seconds, bool playing, bool hasTempo,
                       double tempo);

    /// Main thread. A consistent snapshot for the info window. The mutex is held only for
    /// the string members, briefly; this is never called from the audio thread.
    UiState UiCopy() const;

private:
    /// How long one offline block waits for audio that has not arrived. Generous because a bounce
    /// has no deadline of its own, and bounded because OpenUtau might never answer.
    static constexpr int kOfflineWaitMs = 5000;

    /// How often a moving playhead is reported while the DAW is playing (v1.1). Fast enough
    /// that OpenUtau's own playhead stays close, slow enough that a busy poll round sends
    /// nothing; a state change or a scrub is never throttled.
    static constexpr int64_t kPlayheadIntervalMs = 100;
    /// How far a parked playhead must move before its position is worth a notification.
    static constexpr double kPlayheadScrubMs = 50.0;

    /// How close to the start of the host's timeline the playhead has to be for the host's tempo
    /// to be the project's *base* tempo. See PumpTempo: that is the only value worth reporting,
    /// because it is the only one OpenUtau's guard compares against its own first tempo.
    static constexpr double kTempoAnchorSeconds = 0.1;
    /// How long a value has to hold still before it counts as settled. A tempo that is still
    /// moving is a user drag or a host warming up, and one edit should be one notification.
    static constexpr int64_t kTempoSettleMs = 1000;
    /// Hard floor between two notifications. Whatever a host reports, and however badly it
    /// behaves, the other side never sees more than one of these per interval: OpenUtau shows
    /// each one as a modal dialog, so a stream of them is a way to wedge its UI (2026-11 field
    /// report: a gradual tempo change turned into hundreds of dialogs and a killed process).
    static constexpr int64_t kTempoMinIntervalMs = 5000;
    /// OpenUtau's own matching tolerance is 0.5 BPM, so anything finer is invisible to it —
    /// sending it only costs traffic, and during a ramp it is what makes every poll a "change".
    static constexpr double kTempoEpsilon = 0.5;
    /// Below this, two reports of the same tempo are the same tempo: hosts do not agree on the
    /// exact digits of a constant, and a value that never holds still never settles. Only
    /// movement above it restarts the settle wait.
    static constexpr double kTempoJitter = 0.001;

    /// The worker loop: one round of everything the connection and the host need, then the
    /// snapshot the audio thread reads, then the snapshots nobody is reading any more.
    void Run();
    /// Sends what the audio thread and the host latched: a rate change's re-pull, a track change,
    /// and a playback start nobody has heard about yet.
    void PumpInputs();
    /// Reports the playhead: state changes immediately, movement at the throttle's pace, scrubs
    /// past a threshold. Parked and still is not news.
    void PumpPlayhead();
    /// Reports the base tempo, which is the only tempo the receiver can compare against its own.
    /// The constants above say where it may come from, how still it has to hold and how close two
    /// reports may be; the definition says why.
    void PumpTempo();
    /// One accept-or-poll round: takes a connection if there is none, serves the one there is, and
    /// drops it when the round reports the peer gone.
    void ServeConnection();
    /// Rebuilds the snapshot the audio thread plays, after the layout, the track list or the host
    /// sample rate changed.
    void Republish();

    /// Audio thread, offline only. Sleeps until the worker has everything or until waiting stops
    /// being worthwhile. False when it gave up, and then the block renders with what is there.
    bool WaitForSync();

    // ConnectionHandler, all on the worker thread.
    /// The project baseline: kept for the host session, never rendered from.
    void OnUstx(const std::string &ustx) override;
    /// Replaces the layout wholesale and answers with the hashes whose audio is not held yet.
    std::vector<std::string> OnPartLayout(const std::vector<PartLayout> &parts) override;
    /// The track list: routing for the timeline, names and singers for the window's picker.
    void OnTracks(const std::vector<TrackInfo> &tracks) override;
    /// The project name and saved state the window shows.
    void OnProjectInfo(const ProjectInfo &info) override;
    /// Audio for one hash, converted to the host's rate as it arrives.
    void OnAudio(const std::string &hash, std::vector<uint8_t> &&pcm) override;

    Listener listener_;
    Discovery discovery_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    // --- worker thread only ---
    std::unique_ptr<SocketStream> stream_;
    std::unique_ptr<Connection> connection_;
    AudioStore store_;
    std::vector<PartLayout> layout_;
    std::vector<TrackInfo> tracks_;
    /// The USTX baseline. Nothing renders from it — the audio arrives ready — but it is what a
    /// host session has to carry to reopen the project it was mixed against.
    std::string ustx_;
    int builtTrack_ = -1;
    bool dirty_ = false;

    // --- crossing threads ---
    TimelineBox timeline_;
    std::atomic<double> hostRate_{0.0};
    std::atomic<int> trackNo_{0};
    /// Set by the track picker, consumed by the plugin's process/flush path so the change is
    /// reported to the host as a parameter movement rather than silently applied.
    std::atomic<bool> trackRequestPending_{false};
    /// Set by the audio thread on a rising play edge, cleared by the worker when it sends the
    /// notification. A flag rather than a queue: two starts one poll apart are one start.
    std::atomic<bool> playbackStarted_{false};
    /// The transport as the audio thread last saw it. `positionSeconds_` is meaningful only
    /// when `hasPosition_` holds; likewise `tempo_` under `hasTempo_`.
    std::atomic<bool> hasPosition_{false};
    std::atomic<double> positionSeconds_{0.0};
    std::atomic<bool> playing_{false};
    std::atomic<bool> hasTempo_{false};
    std::atomic<double> tempo_{0.0};
    /// Whether the host is bouncing rather than playing, and whether the worker has everything
    /// the layout names.
    std::atomic<bool> offline_{false};
    std::atomic<bool> synced_{false};
    /// Latched once an offline block has waited its full budget in vain. Without it a bounce of a
    /// project OpenUtau cannot complete would stall for that budget on every block, turning a
    /// missing part into an apparent hang.
    std::atomic<bool> hopeless_{false};
    /// Audio thread only, so the edge is detected where the transport is seen.
    bool wasPlaying_ = false;

    // --- worker thread only: what has already been reported upstream ---
    /// The playhead as last sent. `sentPlaying_` is initialised inverted so that a fresh
    /// connection reports the current state as a change, whatever it is.
    bool sentPlaying_ = true;
    double sentPositionMs_ = 0.0;
    int64_t lastPlayheadMs_ = 0;
    /// The base tempo as last sent, and the value currently being watched at the anchor with the
    /// moment it last changed — a settled value and one still on its way are not the same thing.
    double sentTempo_ = 0.0;
    bool tempoSent_ = false;
    double pendingTempo_ = 0.0;
    bool tempoPending_ = false;
    int64_t pendingTempoSinceMs_ = 0;
    int64_t lastTempoSentMs_ = 0;

    // --- the info window's strings, written on the worker, read on the main thread ---
    mutable std::mutex uiMutex_;
    std::string projectName_;
    bool projectSaved_ = false;
    std::vector<TrackInfo> uiTracks_;
};

}  // namespace bridge
