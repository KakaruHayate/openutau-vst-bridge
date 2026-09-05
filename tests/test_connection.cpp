#include "connection.h"

#include "frame.h"
#include "hash.h"
#include "scripted_stream.h"

#include <doctest.h>

#include <string>
#include <utility>
#include <vector>

using bridge::Connection;
using bridge::ConnectionEnd;
using bridge::ConnectionHandler;
using bridge::ControlKind;
using bridge::ControlLine;
using bridge::PartLayout;
using bridge::ProjectInfo;
using bridge::TrackInfo;
using bridge::test::ScriptedStream;

namespace {

class RecordingHandler final : public ConnectionHandler {
public:
    /// What OnPartLayout reports back as missing, i.e. what the connection will pull.
    std::vector<std::string> missing;

    std::vector<std::string> ustx;
    std::vector<std::vector<PartLayout>> layouts;
    std::vector<std::vector<TrackInfo>> trackUpdates;
    std::vector<ProjectInfo> projectInfos;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> audio;

    void OnUstx(const std::string &document) override { ustx.push_back(document); }

    std::vector<std::string> OnPartLayout(const std::vector<PartLayout> &parts) override {
        layouts.push_back(parts);
        return missing;
    }

    void OnTracks(const std::vector<TrackInfo> &tracks) override {
        trackUpdates.push_back(tracks);
    }

    void OnProjectInfo(const ProjectInfo &info) override { projectInfos.push_back(info); }

    void OnAudio(const std::string &hash, std::vector<uint8_t> &&pcm) override {
        audio.emplace_back(hash, std::move(pcm));
    }
};

/// Stream, handler and connection in one place, so a test reads as a script for the peer.
struct Peer {
    ScriptedStream stream;
    RecordingHandler handler;
    Connection connection{&stream, &handler};

    /// One poll with a short timeout: everything a test needs is already scripted, so there is
    /// nothing to wait for.
    bool Poll() { return connection.Poll(20); }
};

/// Whole stereo float32 frames, distinguishable by value.
std::vector<uint8_t> Pcm(size_t frames) {
    std::vector<uint8_t> bytes(frames * sizeof(float) * 2);
    for (size_t i = 0; i < bytes.size(); i++) {
        bytes[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    return bytes;
}

/// A frame whose header hash is the payload's real hash, as OpenUtau sends it.
std::string AudioFrame(const std::vector<uint8_t> &pcm) {
    std::string hash = bridge::FormatHash(bridge::Xxh64(pcm.data(), pcm.size()));
    return bridge::BuildFrameHeader(hash, pcm.size()) +
           std::string(reinterpret_cast<const char *>(pcm.data()), pcm.size());
}

/// A frame with an arbitrary header hash, for the cases where the two disagree.
std::string RawFrame(const std::string &hash, const std::string &payload) {
    return bridge::BuildFrameHeader(hash, payload.size()) + payload;
}

std::string HashOf(const std::vector<uint8_t> &pcm) {
    return bridge::FormatHash(bridge::Xxh64(pcm.data(), pcm.size()));
}

/// The lines written by the connection, parsed. Ping lines are dropped: a test cares about the
/// answer it provoked, not the heartbeat.
std::vector<ControlLine> Sent(const ScriptedStream &stream) {
    std::vector<ControlLine> parsed;
    for (const std::string &line : stream.WrittenLines()) {
        ControlLine control = bridge::ParseControlLine(line);
        if (control.kind == ControlKind::Notification && control.name == bridge::kind::kPing) {
            continue;
        }
        parsed.push_back(control);
    }
    return parsed;
}

std::string Request(const std::string &uuid, const char *kind, const std::string &payload) {
    return bridge::BuildRequestLine(uuid, kind, payload) + "\n";
}

std::string Notification(const char *kind, const std::string &payload) {
    return bridge::BuildNotificationLine(kind, payload) + "\n";
}

constexpr const char *kInitPayload = R"({"ustx":"name: project"})";
constexpr const char *kLayoutPayload =
    R"({"parts":[{"trackNo":1,"startMs":500,"endMs":1500,"audioHash":"%HASH%"}]})";

std::string LayoutFor(const std::string &hash) {
    std::string payload = kLayoutPayload;
    payload.replace(payload.find("%HASH%"), 6, hash);
    return payload;
}

}  // namespace

TEST_CASE("init hands over the baseline and is answered") {
    Peer peer;
    peer.stream.PushBytes(Request("u-1", bridge::kind::kInit, kInitPayload));

    CHECK(peer.Poll());
    CHECK(peer.connection.IsInitialized());
    REQUIRE(peer.handler.ustx.size() == 1);
    CHECK(peer.handler.ustx[0] == "name: project");

    std::vector<ControlLine> sent = Sent(peer.stream);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].kind == ControlKind::Response);
    CHECK(sent[0].uuid == "u-1");
    CHECK(sent[0].payload.find("\"success\":true") != std::string::npos);
}

TEST_CASE("A malformed init is refused without ending the connection") {
    Peer peer;
    peer.stream.PushBytes(Request("u-1", bridge::kind::kInit, R"({"wrong":1})"));

    CHECK(peer.Poll());
    CHECK(!peer.connection.IsInitialized());
    CHECK(peer.handler.ustx.empty());

    std::vector<ControlLine> sent = Sent(peer.stream);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].payload.find("\"success\":false") != std::string::npos);
}

TEST_CASE("The layout answer goes out before the pull it provokes") {
    std::vector<uint8_t> pcm = Pcm(4);
    std::string hash = HashOf(pcm);
    Peer peer;
    peer.handler.missing = {hash};
    peer.stream.PushBytes(Request("u-2", bridge::kind::kUpdatePartLayout, LayoutFor(hash)));

    CHECK(peer.Poll());
    REQUIRE(peer.handler.layouts.size() == 1);
    REQUIRE(peer.handler.layouts[0].size() == 1);
    CHECK(peer.handler.layouts[0][0].trackNo == 1);
    CHECK(peer.handler.layouts[0][0].startMs == doctest::Approx(500.0));
    CHECK(peer.handler.layouts[0][0].endMs == doctest::Approx(1500.0));
    CHECK(peer.handler.layouts[0][0].audioHash == hash);

    // Order is the whole point: OpenUtau's read loop is blocked on this response, so a pull
    // sent first would sit unanswered until it unblocked.
    std::vector<ControlLine> sent = Sent(peer.stream);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].kind == ControlKind::Response);
    CHECK(sent[0].uuid == "u-2");
    CHECK(sent[0].payload.find(hash) != std::string::npos);
    CHECK(sent[1].kind == ControlKind::Request);
    CHECK(sent[1].name == bridge::kind::kGetAudio);
    CHECK(sent[1].payload.find(hash) != std::string::npos);
}

TEST_CASE("A pulled frame reaches the handler intact") {
    std::vector<uint8_t> pcm = Pcm(64);
    std::string hash = HashOf(pcm);
    Peer peer;
    peer.handler.missing = {hash};
    peer.stream.PushBytes(Request("u-2", bridge::kind::kUpdatePartLayout, LayoutFor(hash)));
    peer.stream.PushBytes(AudioFrame(pcm));

    CHECK(peer.Poll());
    CHECK(peer.Poll());
    REQUIRE(peer.handler.audio.size() == 1);
    CHECK(peer.handler.audio[0].first == hash);
    CHECK(peer.handler.audio[0].second == pcm);
}

TEST_CASE("Pulls are issued one at a time and the queue drains in order") {
    std::vector<uint8_t> first = Pcm(2);
    std::vector<uint8_t> second = Pcm(3);
    Peer peer;
    peer.handler.missing = {HashOf(first), HashOf(second)};
    peer.stream.PushBytes(
        Request("u-2", bridge::kind::kUpdatePartLayout, LayoutFor(HashOf(first))));

    CHECK(peer.Poll());
    std::vector<ControlLine> sent = Sent(peer.stream);
    REQUIRE(sent.size() == 2);  // §6.2: one outstanding getAudio, not two.
    CHECK(sent[1].payload.find(HashOf(first)) != std::string::npos);

    peer.stream.PushBytes(AudioFrame(first));
    CHECK(peer.Poll());
    sent = Sent(peer.stream);
    REQUIRE(sent.size() == 3);
    CHECK(sent[2].name == bridge::kind::kGetAudio);
    CHECK(sent[2].payload.find(HashOf(second)) != std::string::npos);

    peer.stream.PushBytes(AudioFrame(second));
    CHECK(peer.Poll());
    REQUIRE(peer.handler.audio.size() == 2);
    CHECK(peer.handler.audio[0].second == first);
    CHECK(peer.handler.audio[1].second == second);
    CHECK(Sent(peer.stream).size() == 3);  // Nothing left to ask for.
}

TEST_CASE("A frame nobody asked for is dropped without disturbing the outstanding pull") {
    std::vector<uint8_t> wanted = Pcm(2);
    std::vector<uint8_t> other = Pcm(5);
    Peer peer;
    peer.handler.missing = {HashOf(wanted)};
    peer.stream.PushBytes(
        Request("u-2", bridge::kind::kUpdatePartLayout, LayoutFor(HashOf(wanted))));
    peer.stream.PushBytes(AudioFrame(other));
    peer.stream.PushBytes(AudioFrame(wanted));

    CHECK(peer.Poll());
    CHECK(peer.Poll());
    CHECK(peer.handler.audio.empty());
    CHECK(peer.Poll());
    REQUIRE(peer.handler.audio.size() == 1);
    CHECK(peer.handler.audio[0].first == HashOf(wanted));
}

TEST_CASE("A payload that does not match its header hash is dropped, not mixed") {
    std::vector<uint8_t> pcm = Pcm(2);
    std::string hash = HashOf(pcm);
    Peer peer;
    peer.handler.missing = {hash};
    peer.stream.PushBytes(Request("u-2", bridge::kind::kUpdatePartLayout, LayoutFor(hash)));
    // The right length and the requested hash in the header, but not those bytes.
    peer.stream.PushBytes(RawFrame(hash, std::string(pcm.size(), '\x11')));

    CHECK(peer.Poll());
    CHECK(peer.Poll());
    CHECK(peer.handler.audio.empty());
    CHECK(peer.connection.End() == ConnectionEnd::Running);
}

TEST_CASE("A payload that is not whole stereo frames is dropped") {
    Peer peer;
    peer.handler.missing = {"999"};
    peer.stream.PushBytes(Request("u-2", bridge::kind::kUpdatePartLayout, LayoutFor("999")));
    peer.stream.PushBytes(RawFrame("999", std::string(6, '\x00')));

    CHECK(peer.Poll());
    CHECK(peer.Poll());
    CHECK(peer.handler.audio.empty());
    CHECK(peer.connection.End() == ConnectionEnd::Running);
}

TEST_CASE("A refusal envelope frees the pull immediately") {
    std::vector<uint8_t> second = Pcm(3);
    Peer peer;
    peer.handler.missing = {"999", HashOf(second)};
    peer.stream.PushBytes(Request("u-2", bridge::kind::kUpdatePartLayout, LayoutFor("999")));
    CHECK(peer.Poll());

    std::vector<ControlLine> sent = Sent(peer.stream);
    REQUIRE(sent.size() == 2);
    std::string pullUuid = sent[1].uuid;
    REQUIRE(!pullUuid.empty());

    // Waiting out the ten-second timeout would cost nothing but delay.
    peer.stream.PushBytes("response:" + pullUuid + R"( {"success":false,"error":"no such part"})" +
                          "\n");
    CHECK(peer.Poll());
    sent = Sent(peer.stream);
    REQUIRE(sent.size() == 3);
    CHECK(sent[2].name == bridge::kind::kGetAudio);
    CHECK(sent[2].payload.find(HashOf(second)) != std::string::npos);
}

TEST_CASE("updateTracks reaches the handler in OpenUtau's own scale") {
    Peer peer;
    peer.stream.PushBytes(Notification(
        bridge::kind::kUpdateTracks,
        R"({"tracks":[{"name":"Lead","volume":-6.0,"pan":-100,"muted":false},)"
        R"({"name":"Harmony","volume":0,"pan":50,"muted":true}]})"));

    CHECK(peer.Poll());
    REQUIRE(peer.handler.trackUpdates.size() == 1);
    const std::vector<TrackInfo> &tracks = peer.handler.trackUpdates[0];
    REQUIRE(tracks.size() == 2);
    CHECK(tracks[0].name == "Lead");
    CHECK(tracks[0].volume == doctest::Approx(-6.0));
    CHECK(tracks[0].pan == doctest::Approx(-100.0));
    CHECK(!tracks[0].muted);
    CHECK(tracks[1].muted);
    CHECK(Sent(peer.stream).empty());  // A notification is never answered.
}

TEST_CASE("An unknown request is refused and an unknown notification ignored") {
    Peer peer;
    peer.stream.PushBytes(Request("u-9", "updatePartLayoutV2", "{}"));
    peer.stream.PushBytes(Notification("somethingNewer", R"({"whatever":1})"));
    peer.stream.PushBytes("this is not a control line at all\n");

    CHECK(peer.Poll());
    CHECK(peer.Poll());
    CHECK(peer.Poll());
    CHECK(peer.connection.End() == ConnectionEnd::Running);

    // Only the unknown *request* owes an answer; the rest are append-only versioning (§5.1, §10).
    std::vector<ControlLine> sent = Sent(peer.stream);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].kind == ControlKind::Response);
    CHECK(sent[0].uuid == "u-9");
    CHECK(sent[0].payload.find("\"success\":false") != std::string::npos);
}

TEST_CASE("The session can ask for audio outside a layout exchange") {
    // What a host sample-rate change needs: the clips are invalid, the layout still names them,
    // and no message asks OpenUtau to re-send a layout (§6.2).
    std::vector<uint8_t> pcm = Pcm(2);
    Peer peer;
    peer.connection.RequestAudio({HashOf(pcm), "", HashOf(pcm)});

    std::vector<ControlLine> sent = Sent(peer.stream);
    REQUIRE(sent.size() == 1);  // One outstanding pull, and the blank hash is not one.
    CHECK(sent[0].name == bridge::kind::kGetAudio);
    CHECK(sent[0].payload.find(HashOf(pcm)) != std::string::npos);

    peer.stream.PushBytes(AudioFrame(pcm));
    CHECK(peer.Poll());
    REQUIRE(peer.handler.audio.size() == 1);
    // The repeat was dropped rather than queued: pulling the same part twice would cost a
    // second copy of it for nothing.
    CHECK(Sent(peer.stream).size() == 1);
}

TEST_CASE("Each way a connection ends is distinguished") {
    SUBCASE("the peer sends close") {
        Peer peer;
        peer.handler.missing = {"999"};
        peer.stream.PushBytes(
            Request("u-2", bridge::kind::kUpdatePartLayout, LayoutFor("999")));
        peer.stream.PushBytes("close\n");

        CHECK(peer.Poll());
        CHECK(!peer.Poll());
        CHECK(peer.connection.End() == ConnectionEnd::PeerClosed);
        CHECK(peer.stream.WasShutdown());
        CHECK(!peer.connection.Poll(20));  // Ended stays ended.
    }
    SUBCASE("the socket closes") {
        Peer peer;
        peer.stream.PushEnd();

        CHECK(!peer.Poll());
        CHECK(peer.connection.End() == ConnectionEnd::StreamClosed);
    }
    SUBCASE("a frame header cannot be parsed") {
        // The payload length is unknown, so there is no way back to a frame boundary (§8).
        Peer peer;
        peer.stream.PushBytes("audio 999 4 extra\n");

        CHECK(!peer.Poll());
        CHECK(peer.connection.End() == ConnectionEnd::ProtocolError);
        CHECK(peer.connection.EndDetail().find("audio 999 4 extra") != std::string::npos);
    }
    SUBCASE("a write fails") {
        Peer peer;
        peer.stream.FailWrites();
        peer.stream.PushBytes(Request("u-1", bridge::kind::kInit, kInitPayload));

        CHECK(!peer.Poll());
        CHECK(peer.connection.End() == ConnectionEnd::StreamClosed);
    }
    SUBCASE("we close locally") {
        Peer peer;
        peer.connection.Close();

        CHECK(peer.connection.End() == ConnectionEnd::LocalClose);
        CHECK(peer.stream.WasShutdown());
        CHECK(!peer.Poll());
    }
}
