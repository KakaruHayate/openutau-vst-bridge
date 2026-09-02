#pragma once

/*
 * One connection, driven entirely from the worker thread. Reading, dispatching and pulling
 * all happen in Poll(), which is what keeps the design free of nested waits: a pull is a
 * queue plus one outstanding request, not a blocking call that would have to re-enter the
 * read loop to see its own answer.
 *
 * Only ever touched by the worker thread. Anything the audio thread wants sent is queued as
 * a flag in the session and sent from here on the next poll.
 */

#include "messages.h"
#include "reader.h"
#include "stream.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace bridge {

/// What the session does with what a connection decodes. Worker thread only.
class ConnectionHandler {
public:
    virtual ~ConnectionHandler() = default;

    /// The baseline from `init`, or a later whole-document push from `updateUstx`.
    virtual void OnUstx(const std::string &ustx) = 0;

    /// Returns the hashes not already held; they become this connection's pull queue.
    virtual std::vector<std::string> OnPartLayout(const std::vector<PartLayout> &parts) = 0;

    virtual void OnTracks(const std::vector<TrackInfo> &tracks) = 0;

    /// A payload that has already passed its hash and frame-size checks.
    virtual void OnAudio(const std::string &hash, std::vector<uint8_t> &&pcm) = 0;
};

enum class ConnectionEnd {
    Running,
    /// The peer sent a bare `close` (§5.1).
    PeerClosed,
    StreamClosed,
    ProtocolError,
    LocalClose,
};

class Connection {
public:
    /// PROTOCOL.md §3: the plugin owns the heartbeat send interval.
    static constexpr int64_t kPingIntervalMs = 5000;

    /// A pull that goes unanswered this long is abandoned rather than fatal — the next
    /// `updatePartLayout` lists the hash as missing again, which is a free retry.
    static constexpr int64_t kPullTimeoutMs = 10000;

    Connection(ByteStream *stream, ConnectionHandler *handler);

    /// One step: send what is due, read what is there, dispatch it. False once ended.
    bool Poll(int timeoutMs);

    bool SendNotification(const char *kind, const std::string &json);

    /// Queues pulls outside a layout exchange. The host changing its sample rate invalidates
    /// every converted clip while the layout still names them, and §6.2 has no way to ask
    /// OpenUtau to re-send a layout — asking for the hashes again is the whole mechanism.
    void RequestAudio(const std::vector<std::string> &hashes);

    /// True once `init` has been answered, i.e. the baseline has arrived.
    bool IsInitialized() const { return initialized_; }

    ConnectionEnd End() const { return end_; }
    const std::string &EndDetail() const { return endDetail_; }

    /// Drops the connection locally. The plugin never sends `close`; that direction is
    /// OpenUtau's alone (§5.1).
    void Close();

private:
    void Dispatch(const std::string &line);
    void ServeRequest(const std::string &uuid, const std::string &kind, const std::string &payload);
    void HandleNotification(const std::string &kind, const std::string &payload);
    void HandleResponse(const std::string &uuid, const std::string &payload);
    bool ReceiveAudioFrame(const std::string &header);

    bool SendLine(const std::string &line);
    bool Respond(const std::string &uuid, const std::string &envelope);
    void StartNextPull();
    void Finish(ConnectionEnd end, std::string detail);

    ByteStream *stream_;
    ConnectionHandler *handler_;
    FrameReader reader_;

    ConnectionEnd end_ = ConnectionEnd::Running;
    std::string endDetail_;
    bool initialized_ = false;

    int64_t nextPingMs_ = 0;

    std::deque<std::string> pendingPulls_;
    /// Empty when nothing is outstanding — §6.2 allows one pull at a time in v1.
    std::string pullHash_;
    std::string pullUuid_;
    int64_t pullDeadlineMs_ = 0;
};

}  // namespace bridge
