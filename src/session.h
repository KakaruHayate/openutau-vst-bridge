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

    Session() = default;

    /// The discovery directory is injectable for the same reason Discovery's is: a test that
    /// advertised itself in the real one would be found, and connected to, by a running OpenUtau.
    explicit Session(std::filesystem::path discoveryDirectory)
        : discovery_(std::move(discoveryDirectory)) {}

    ~Session() override;

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    /// Main thread. Binds a port, advertises it, then starts the worker — in that order,
    /// because OpenUtau reads an advertisement as a promise that the port is already listening
    /// (§4). False if the port could not be bound or advertised, and then nothing is running.
    bool Start();

    /// Main thread. Idempotent, and safe to call on a session that never started.
    void Stop();

    bool IsRunning() const { return running_.load(); }

    /// 0 until Start() succeeds.
    int Port() const { return listener_.Port(); }

    bool IsConnected() const { return connected_.load(); }

    /// The host's rate, from `activate`. Clips are converted on arrival, so a change drops
    /// every one of them and the worker re-pulls what the layout still names.
    void SetHostSampleRate(double rate) { hostRate_.store(rate); }

    /// Which OpenUtau track this instance routes. OpenUtau's volume/pan/mute fields remain on the
    /// wire for compatibility, but the bridge emits pre-fader audio and the DAW owns those controls.
    void SetTrackNo(int trackNo) { trackNo_.store(trackNo); }
    int TrackNo() const { return trackNo_.load(); }

    /// Main thread, from the track picker. Changes the routing immediately - the audio follows
    /// on the next block - and records a pending request so the plugin can report the new value
    /// to the host as a parameter change (CLAP: gesture + value + gesture on the output queue).
    /// The pending flag is what keeps a host-side automation and a GUI click from echoing.
    void RequestTrackNo(int trackNo) {
        trackNo_.store(trackNo);
        trackRequestPending_.store(true);
    }

    /// Main thread, from the plugin's process/flush path. True exactly once per GUI change;
    /// the plugin answers it by notifying the host for the parameter identified below.
    bool ConsumeTrackRequest() { return trackRequestPending_.exchange(false); }

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
    /// Tempo changes below this are noise; OpenUtau's own matching tolerance is 0.5 BPM.
    static constexpr double kTempoEpsilon = 0.01;

    void Run();
    void PumpInputs();
    void PumpPlayhead();
    void PumpTempo();
    void ServeConnection();
    void Republish();

    /// Audio thread, offline only. Sleeps until the worker has everything or until waiting stops
    /// being worthwhile. False when it gave up, and then the block renders with what is there.
    bool WaitForSync();

    // ConnectionHandler, all on the worker thread.
    void OnUstx(const std::string &ustx) override;
    std::vector<std::string> OnPartLayout(const std::vector<PartLayout> &parts) override;
    void OnTracks(const std::vector<TrackInfo> &tracks) override;
    void OnProjectInfo(const ProjectInfo &info) override;
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
    double sentTempo_ = 0.0;
    bool tempoSent_ = false;

    // --- the info window's strings, written on the worker, read on the main thread ---
    mutable std::mutex uiMutex_;
    std::string projectName_;
    bool projectSaved_ = false;
    std::vector<TrackInfo> uiTracks_;
};

}  // namespace bridge
