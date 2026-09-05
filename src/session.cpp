#include "session.h"

#include "clock.h"
#include "log.h"
#include "messages.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <utility>

namespace bridge {

Session::~Session() {
    Stop();
}

bool Session::Start() {
    if (running_.load()) {
        return true;
    }
    if (!listener_.Start()) {
        return false;
    }
    // Bound first, advertised second: OpenUtau reads an advertisement as a promise that the port
    // is already listening, and deletes any file whose port it can bind itself (§4). The display
    // name is left to the stem, since a host does not say which track a plugin is on until later.
    if (!discovery_.Publish(listener_.Port(), std::string())) {
        listener_.Stop();
        return false;
    }
    stop_.store(false);
    running_.store(true);
    worker_ = std::thread([this] { Run(); });
    BRIDGE_INFO("Bridge listening on port %d.", listener_.Port());
    return true;
}

void Session::Stop() {
    if (!running_.load()) {
        return;
    }
    stop_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    // After the worker is gone: an advertisement must never outlive the thread that would
    // answer on it.
    discovery_.Remove();
    listener_.Stop();
    connection_.reset();
    stream_.reset();
    connected_.store(false);
    running_.store(false);
}

void Session::Run() {
    while (!stop_.load()) {
        PumpInputs();
        ServeConnection();
        PumpPlayhead();
        PumpTempo();
        if (dirty_) {
            dirty_ = false;
            Republish();
        }
        // Stored after the snapshot it describes, so a bounce that sees "synced" is looking at a
        // timeline that already holds every part rather than one about to be replaced.
        synced_.store(store_.Missing(layout_).empty());
        // Retired snapshots are freed here, on this thread: releasing one can take a lock inside
        // the allocator, and the audio thread is the one thread that must never wait for it.
        timeline_.Collect();
    }
}

void Session::PumpInputs() {
    double rate = hostRate_.load();
    if (rate > 0.0 && rate != store_.HostSampleRate()) {
        BRIDGE_INFO("Host sample rate is %.0f Hz; every part is converted again at it.", rate);
        store_.SetHostSampleRate(rate);
        dirty_ = true;
        if (connection_ != nullptr) {
            // The layout still names the parts and §6.2 has no message for asking OpenUtau to
            // re-send one, so the hashes are the whole request.
            connection_->RequestAudio(store_.Missing(layout_));
        } else if (!layout_.empty()) {
            BRIDGE_WARN("Nothing is connected to re-pull %zu parts from; they are silent until "
                        "OpenUtau comes back.",
                        layout_.size());
        }
    }
    int track = trackNo_.load();
    if (track != builtTrack_) {
        builtTrack_ = track;
        dirty_ = true;
    }
    // A start nobody is connected to hear is not owed a notification, so the flag is consumed
    // either way rather than held for a connection that may never arrive.
    if (playbackStarted_.exchange(false) && connection_ != nullptr) {
        connection_->SendNotification(kind::kPlaybackStarted, BuildEmptyPayload());
    }
}

void Session::ServeConnection() {
    if (connection_ != nullptr) {
        if (connection_->Poll(kPollMs)) {
            return;
        }
        BRIDGE_INFO("Connection ended: %s", connection_->EndDetail().c_str());
        connection_.reset();
        stream_.reset();
        connected_.store(false);
        // The snapshot and the audio behind it are kept: a project whose editor has closed is
        // still a project the DAW can play and mix.
        return;
    }
    std::unique_ptr<SocketStream> accepted = listener_.Accept(kPollMs);
    if (!accepted) {
        return;
    }
    BRIDGE_INFO("OpenUtau connected on port %d.", listener_.Port());
    stream_ = std::move(accepted);
    // Cast here rather than in make_unique: the handler base is private, so only a member of
    // this class may perform the conversion.
    connection_ = std::make_unique<Connection>(stream_.get(),
                                               static_cast<ConnectionHandler *>(this));
    connected_.store(true);
    // A fresh connection knows nothing of what was reported before it. Inverting the last-sent
    // play state makes the current one a change, so position and tempo are reported immediately
    // rather than after their first interval.
    sentPlaying_ = !playing_.load();
    sentPositionMs_ = 0.0;
    lastPlayheadMs_ = 0;
    tempoSent_ = false;
}

void Session::Republish() {
    // The store's rate rather than the host's: it is the rate the clips have actually been
    // converted to, so a snapshot can never claim one its audio is not at.
    timeline_.Publish(
        BuildTimeline(layout_, tracks_, store_, builtTrack_, store_.HostSampleRate()));
}

void Session::OnUstx(const std::string &ustx) {
    ustx_ = ustx;
    BRIDGE_INFO("Project baseline received, %zu bytes.", ustx.size());
}

std::vector<std::string> Session::OnPartLayout(const std::vector<PartLayout> &parts) {
    layout_ = parts;
    std::vector<std::string> missing = store_.Missing(parts);
    std::vector<std::string> named;
    named.reserve(parts.size());
    for (const PartLayout &part : parts) {
        if (!part.audioHash.empty()) {
            named.push_back(part.audioHash);
        }
    }
    // A layout is the whole truth about the project, so what it does not name is a part that was
    // deleted or re-rendered — and part audio is by far the largest thing this plugin holds.
    store_.Retain(named);
    dirty_ = true;
    return missing;
}

void Session::OnTracks(const std::vector<TrackInfo> &tracks) {
    tracks_ = tracks;
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        trackNames_.clear();
        trackNames_.reserve(tracks.size());
        for (const TrackInfo &track : tracks) {
            trackNames_.push_back(track.name);
        }
    }
    dirty_ = true;
}

void Session::OnProjectInfo(const ProjectInfo &info) {
    {
        std::lock_guard<std::mutex> lock(uiMutex_);
        projectName_ = info.name;
        projectSaved_ = info.saved;
    }
    BRIDGE_INFO("Project is \"%s\" (%s).", info.name.c_str(),
                info.saved ? "saved" : "unsaved");
}

void Session::PumpPlayhead() {
    // A pending playbackStarted goes out first (PumpInputs sends it). Reporting a position
    // ahead of the start it belongs to would scramble the order at the receiver, so this
    // waits a round rather than racing the flag.
    if (playbackStarted_.load()) {
        return;
    }
    if (connection_ == nullptr || !hasPosition_.load()) {
        return;
    }
    bool playing = playing_.load();
    double positionMs = positionSeconds_.load() * 1000.0;
    int64_t now = NowMs();
    bool due = false;
    if (playing != sentPlaying_) {
        // A state change is never throttled: it is what tells OpenUtau the DAW has started,
        // stopped or begun scrubbing.
        due = true;
    } else if (playing) {
        due = now - lastPlayheadMs_ >= kPlayheadIntervalMs;
    } else {
        // Parked: only movement is news, so a scrub is reported and a still playhead is not.
        due = std::abs(positionMs - sentPositionMs_) > kPlayheadScrubMs;
    }
    if (!due) {
        return;
    }
    if (connection_->SendNotification(kind::kPlayhead,
                                      BuildPlayheadPayload(positionMs, playing))) {
        sentPlaying_ = playing;
        sentPositionMs_ = positionMs;
        lastPlayheadMs_ = now;
    }
}

void Session::PumpTempo() {
    if (connection_ == nullptr || !hasTempo_.load()) {
        return;
    }
    double tempo = tempo_.load();
    if (tempoSent_ && std::abs(tempo - sentTempo_) <= kTempoEpsilon) {
        return;
    }
    if (connection_->SendNotification(kind::kBpm, BuildBpmPayload(tempo))) {
        sentTempo_ = tempo;
        tempoSent_ = true;
    }
}

void Session::OnAudio(const std::string &hash, std::vector<uint8_t> &&pcm) {
    if (!store_.Insert(hash, pcm)) {
        BRIDGE_WARN("Audio for %s was not stored, so its part stays silent.", hash.c_str());
        return;
    }
    dirty_ = true;
}

void Session::Render(int64_t fromFrame, size_t frames, float *left, float *right) {
    if (frames == 0) {
        return;
    }
    // Assigning, not adding: this is the only source of the plugin's output, and a host is not
    // required to hand over a cleared buffer.
    std::memset(left, 0, frames * sizeof(float));
    std::memset(right, 0, frames * sizeof(float));
    if (offline_.load() && !synced_.load() && !hopeless_.load()) {
        // Only ever here on a bounce, where this thread has no deadline. In real time the check
        // above is two atomic loads and nothing more.
        WaitForSync();
    }
    // Scoped to the block on purpose. A borrow held any longer would stop the worker from ever
    // freeing the snapshot it replaced.
    TimelineBox::Borrow borrow(timeline_);
    if (!borrow) {
        return;
    }
    MixTimeline(*borrow.Get(), fromFrame, frames, left, right);
}

bool Session::WaitForSync() {
    int64_t deadline = NowMs() + kOfflineWaitMs;
    while (NowMs() < deadline) {
        if (synced_.load()) {
            return true;
        }
        if (!connected_.load()) {
            // Nobody is going to send the rest of it. A project whose editor has been closed
            // still bounces the audio it has, which is the same promise playback makes.
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BRIDGE_WARN("Waited %d ms for the rest of the project and it did not arrive; the bounce goes "
                "on with what is held.",
                kOfflineWaitMs);
    hopeless_.store(true);
    return false;
}

void Session::NoteTransport(bool hasPosition, double seconds, bool playing, bool hasTempo,
                            double tempo) {
    hasPosition_.store(hasPosition);
    positionSeconds_.store(seconds);
    playing_.store(playing);
    hasTempo_.store(hasTempo);
    tempo_.store(tempo);
    if (playing && !wasPlaying_) {
        playbackStarted_.store(true);
    }
    wasPlaying_ = playing;
}

UiState Session::UiCopy() const {
    UiState state;
    state.connected = connected_.load();
    state.port = listener_.Port();
    state.trackNo = trackNo_.load();
    state.hasTempo = hasTempo_.load();
    state.tempo = tempo_.load();
    state.playing = playing_.load();
    std::lock_guard<std::mutex> lock(uiMutex_);
    state.projectName = projectName_;
    state.projectSaved = projectSaved_;
    state.trackNames = trackNames_;
    return state;
}

}  // namespace bridge
