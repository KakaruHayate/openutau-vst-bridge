#pragma once

/*
 * One plugin instance's whole bridge: the listener, its advertisement, the connection when
 * there is one, the audio it has collected and the snapshot the audio thread plays.
 *
 * Thread rules, which are the reason this class exists at all:
 *   - The worker thread owns the connection, the store and the layout. Nothing else touches
 *     them, so none of them needs a lock.
 *   - The audio thread calls Render() and NotePlaying(). Both are wait-free.
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
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bridge {

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

    /// Which OpenUtau track this instance plays. Parts on any other track are not this
    /// plugin's audio, and the track's fader is what its gain comes from.
    void SetTrackNo(int trackNo) { trackNo_.store(trackNo); }
    int TrackNo() const { return trackNo_.load(); }

    /// Main thread. Whether the host is rendering faster than real time — a bounce, a freeze, an
    /// export. Audio arrives over a socket while the timeline is being played, which is fine at
    /// real-time speed and not fine at a bounce's, so offline Render() is allowed to wait for
    /// what has not arrived. Cleared again when the host goes back to real time.
    void SetOffline(bool offline) {
        offline_.store(offline);
        hopeless_.store(false);  // A new pass deserves a fresh chance to wait.
    }

    /// True when every part the layout names has its audio, converted and placed. This is what a
    /// bounce waits for, and the honest answer to "is what I would render now complete".
    bool IsSynced() const { return synced_.load(); }

    /// Audio thread. Fills the buffers with what the timeline holds over
    /// [fromFrame, fromFrame + frames) — assigns rather than adds, since this is the only
    /// source of the plugin's output. Silence when nothing is connected or nothing is placed.
    void Render(int64_t fromFrame, size_t frames, float *left, float *right);

    /// Audio thread. The transport's play state, every block. Its rising edge is what
    /// `playbackStarted` reports (§6.1), so OpenUtau can prioritise rendering what is about to
    /// be heard; the notification itself is sent from the worker.
    void NotePlaying(bool playing);

private:
    /// How long one offline block waits for audio that has not arrived. Generous because a bounce
    /// has no deadline of its own, and bounded because OpenUtau might never answer.
    static constexpr int kOfflineWaitMs = 5000;

    void Run();
    void PumpInputs();
    void ServeConnection();
    void Republish();

    /// Audio thread, offline only. Sleeps until the worker has everything or until waiting stops
    /// being worthwhile. False when it gave up, and then the block renders with what is there.
    bool WaitForSync();

    // ConnectionHandler, all on the worker thread.
    void OnUstx(const std::string &ustx) override;
    std::vector<std::string> OnPartLayout(const std::vector<PartLayout> &parts) override;
    void OnTracks(const std::vector<TrackInfo> &tracks) override;
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
    /// Set by the audio thread on a rising play edge, cleared by the worker when it sends the
    /// notification. A flag rather than a queue: two starts one poll apart are one start.
    std::atomic<bool> playbackStarted_{false};
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
};

}  // namespace bridge
