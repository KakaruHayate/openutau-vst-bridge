#include "connection.h"

#include "clock.h"
#include "frame.h"
#include "hash.h"
#include "log.h"

#include <utility>
#include <algorithm>

namespace bridge {

Connection::Connection(ByteStream *stream, ConnectionHandler *handler)
    : stream_(stream), handler_(handler), reader_(stream) {
    nextPingMs_ = NowMs() + kPingIntervalMs;
}

bool Connection::Poll(int timeoutMs) {
    if (end_ != ConnectionEnd::Running) {
        return false;
    }
    int64_t now = NowMs();
    if (now >= nextPingMs_) {
        nextPingMs_ = now + kPingIntervalMs;
        if (!SendNotification(kind::kPing, BuildEmptyPayload())) {
            return false;
        }
    }
    if (!pullHash_.empty() && now >= pullDeadlineMs_) {
        BRIDGE_WARN("Audio pull for hash %s went unanswered; abandoning it.", pullHash_.c_str());
        pullHash_.clear();
        pullUuid_.clear();
        StartNextPull();
        if (end_ != ConnectionEnd::Running) {
            return false;
        }
    }

    std::string line;
    ReadStatus status = reader_.ReadLine(&line, timeoutMs);
    switch (status) {
        case ReadStatus::Ok:
            break;
        case ReadStatus::Timeout:
            return true;
        case ReadStatus::EndOfStream:
            Finish(ConnectionEnd::StreamClosed, "Peer closed the socket.");
            return false;
        case ReadStatus::ProtocolError:
            Finish(ConnectionEnd::ProtocolError, reader_.Error());
            return false;
    }
    Dispatch(line);
    return end_ == ConnectionEnd::Running;
}

void Connection::Dispatch(const std::string &line) {
    // §5.2: a line starting with "audio " introduces binary; everything else is control.
    if (IsFrameHeader(line)) {
        if (!ReceiveAudioFrame(line)) {
            return;
        }
        StartNextPull();
        return;
    }
    ControlLine control = ParseControlLine(line);
    switch (control.kind) {
        case ControlKind::Close:
            Finish(ConnectionEnd::PeerClosed, "Peer sent close.");
            return;
        case ControlKind::Request:
            ServeRequest(control.uuid, control.name, control.payload);
            return;
        case ControlKind::Notification:
            HandleNotification(control.name, control.payload);
            return;
        case ControlKind::Response:
            HandleResponse(control.uuid, control.payload);
            return;
        case ControlKind::Unknown:
            // Malformed control lines are logged and survived, never fatal (§8).
            BRIDGE_WARN("Unrecognized control line, ignored: %.120s", line.c_str());
            return;
    }
}

void Connection::ServeRequest(const std::string &uuid, const std::string &kindName,
                             const std::string &payload) {
    if (kindName == kind::kInit) {
        std::string ustx;
        if (!ParseUstxPayload(payload, &ustx)) {
            Respond(uuid, BuildFailEnvelope("init payload carried no ustx document."));
            return;
        }
        handler_->OnUstx(ustx);
        initialized_ = true;
        Respond(uuid, BuildInitResponseEnvelope());
        return;
    }
    if (kindName == kind::kUpdatePartLayout) {
        std::vector<PartLayout> parts;
        if (!ParsePartLayoutRequest(payload, &parts)) {
            Respond(uuid, BuildFailEnvelope("updatePartLayout payload was malformed."));
            return;
        }
        std::vector<std::string> missing = handler_->OnPartLayout(parts);
        // Answer before pulling: the peer is blocked on this response, and a pull can only
        // be served once its read loop is free again.
        if (!Respond(uuid, BuildPartLayoutResponseEnvelope(missing))) {
            return;
        }
        RequestAudio(missing);
        return;
    }
    // An unknown kind is refused with a failed envelope, never by dropping the connection (§5.1).
    BRIDGE_WARN("Unsupported request kind '%s', refused.", kindName.c_str());
    Respond(uuid, BuildFailEnvelope("Unsupported request kind: " + kindName));
}

void Connection::HandleNotification(const std::string &kindName, const std::string &payload) {
    if (kindName == kind::kUpdateUstx) {
        std::string ustx;
        if (!ParseUstxPayload(payload, &ustx)) {
            BRIDGE_WARN("updateUstx carried no ustx document, ignored.");
            return;
        }
        handler_->OnUstx(ustx);
        return;
    }
    if (kindName == kind::kUpdateTracks) {
        std::vector<TrackInfo> tracks;
        if (!ParseTracksNotification(payload, &tracks)) {
            BRIDGE_WARN("updateTracks was malformed, ignored.");
            return;
        }
        handler_->OnTracks(tracks);
        return;
    }
    if (kindName == kind::kUpdateProjectInfo) {
        ProjectInfo info;
        if (!ParseProjectInfoNotification(payload, &info)) {
            BRIDGE_WARN("updateProjectInfo was malformed, ignored.");
            return;
        }
        handler_->OnProjectInfo(info);
        return;
    }
    // Unknown notification kinds are logged and ignored, which is what append-only
    // versioning requires of an older peer (§5.1, §10).
    BRIDGE_INFO("Ignoring unknown notification '%s'.", kindName.c_str());
}

void Connection::HandleResponse(const std::string &uuid, const std::string &payload) {
    if (pullUuid_.empty() || uuid != pullUuid_) {
        BRIDGE_WARN("Response for unknown request %s, ignored.", uuid.c_str());
        return;
    }
    // The only requests this side sends are getAudio, whose success arrives as a data frame.
    // An envelope on that uuid therefore means a refusal, and waiting out the timeout would
    // cost nothing but delay.
    Envelope envelope;
    std::string reason = ParseEnvelope(payload, &envelope) && !envelope.error.empty()
        ? envelope.error
        : "(no reason given)";
    BRIDGE_WARN("Peer refused getAudio for %s: %s", pullHash_.c_str(), reason.c_str());
    pullHash_.clear();
    pullUuid_.clear();
    StartNextPull();
}

bool Connection::ReceiveAudioFrame(const std::string &header) {
    std::string hash;
    int32_t length = 0;
    if (!TryParseFrameHeader(header, &hash, &length)) {
        // The payload length is unknown, so the stream position is lost past recovery (§8).
        Finish(ConnectionEnd::ProtocolError, "Malformed audio frame header: " + header);
        return false;
    }
    std::vector<uint8_t> payload;
    if (reader_.ReadExactly(static_cast<size_t>(length), &payload) != ReadStatus::Ok) {
        Finish(ConnectionEnd::ProtocolError, reader_.Error());
        return false;
    }
    if (hash != pullHash_) {
        BRIDGE_WARN("Unsolicited audio frame for hash %s (%d bytes), dropped.",
                    hash.c_str(), length);
        return true;
    }
    pullHash_.clear();
    pullUuid_.clear();

    // §6.1: the frame length is a cheap cross-check, and the payload has to be whole stereo
    // float32 frames or it cannot be interpreted at all.
    constexpr size_t kBytesPerFrame = sizeof(float) * static_cast<size_t>(kWireChannels);
    if (payload.size() % kBytesPerFrame != 0) {
        BRIDGE_WARN("Audio payload for %s is %zu bytes, not whole stereo float32 frames; dropped.",
                    hash.c_str(), payload.size());
        return true;
    }
    uint64_t actual = Xxh64(payload.data(), payload.size());
    if (FormatHash(actual) != hash) {
        BRIDGE_WARN("Audio payload for %s hashed to %s; dropped.", hash.c_str(),
                    FormatHash(actual).c_str());
        return true;
    }
    handler_->OnAudio(hash, std::move(payload));
    return true;
}

void Connection::StartNextPull() {
    if (!pullHash_.empty() || pendingPulls_.empty() || end_ != ConnectionEnd::Running) {
        return;
    }
    std::string hash = std::move(pendingPulls_.front());
    pendingPulls_.pop_front();
    std::string uuid = NewUuid();
    if (!SendLine(BuildRequestLine(uuid, kind::kGetAudio, BuildGetAudioPayload(hash)))) {
        return;
    }
    pullHash_ = std::move(hash);
    pullUuid_ = std::move(uuid);
    pullDeadlineMs_ = NowMs() + kPullTimeoutMs;
}

bool Connection::SendNotification(const char *kindName, const std::string &json) {
    return SendLine(BuildNotificationLine(kindName, json));
}

void Connection::RequestAudio(const std::vector<std::string> &hashes) {
    for (const std::string &hash : hashes) {
        if (hash.empty() || hash == pullHash_) {
            continue;
        }
        // Two layouts in a row name the same missing hash; pulling it twice would cost a second
        // copy of the whole part for nothing. The queue is one entry per part, so a scan is free.
        if (std::find(pendingPulls_.begin(), pendingPulls_.end(), hash) != pendingPulls_.end()) {
            continue;
        }
        pendingPulls_.push_back(hash);
    }
    StartNextPull();
}

bool Connection::Respond(const std::string &uuid, const std::string &envelope) {
    return SendLine(BuildResponseLine(uuid, envelope));
}

bool Connection::SendLine(const std::string &line) {
    if (end_ != ConnectionEnd::Running) {
        return false;
    }
    std::string framed = line;
    framed.push_back('\n');
    if (!stream_->Write(framed.data(), framed.size())) {
        Finish(ConnectionEnd::StreamClosed, "Write failed.");
        return false;
    }
    return true;
}

void Connection::Close() {
    Finish(ConnectionEnd::LocalClose, "Closed locally.");
}

void Connection::Finish(ConnectionEnd end, std::string detail) {
    if (end_ != ConnectionEnd::Running) {
        return;
    }
    end_ = end;
    endDetail_ = std::move(detail);
    pendingPulls_.clear();
    pullHash_.clear();
    pullUuid_.clear();
    stream_->Shutdown();
}

}  // namespace bridge
