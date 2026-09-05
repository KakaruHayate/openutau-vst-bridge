#include "session.h"

#include "clock.h"
#include "frame.h"
#include "hash.h"
#include "loopback.h"
#include "messages.h"
#include "reader.h"
#include "temp_dir.h"

#include <doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using bridge::ControlKind;
using bridge::ControlLine;
using bridge::FrameReader;
using bridge::ReadStatus;
using bridge::Session;
using bridge::SocketStream;
using bridge::test::FilesIn;
using bridge::test::kWaitMs;
using bridge::test::ReadAll;
using bridge::test::TempDir;
using bridge::test::WirePcm;
using bridge::test::WriteLine;

namespace fs = std::filesystem;
namespace kind = bridge::kind;

namespace {

/// The test playing OpenUtau: it dials the advertised port and speaks the protocol by hand, so
/// nothing between the two sides is simulated.
class FakeOpenUtau {
public:
    bool Connect(int port) {
        stream_ = bridge::ConnectLoopback(port);
        if (stream_ == nullptr) {
            return false;
        }
        reader_ = std::make_unique<FrameReader>(stream_.get());
        return true;
    }

    bool Send(const std::string &line) { return WriteLine(stream_.get(), line); }

    bool SendFrame(const std::string &hash, const std::vector<uint8_t> &pcm) {
        std::string header = bridge::BuildFrameHeader(hash, pcm.size());
        return stream_->Write(header.data(), header.size()) &&
               stream_->Write(pcm.data(), pcm.size());
    }

    /// The next line that is not a heartbeat. Kind Unknown on timeout, which no test expects.
    ControlLine ReadControl(int timeoutMs = kWaitMs) {
        std::string line;
        while (reader_->ReadLine(&line, timeoutMs) == ReadStatus::Ok) {
            ControlLine control = bridge::ParseControlLine(line);
            if (control.kind == ControlKind::Notification && control.name == kind::kPing) {
                continue;
            }
            return control;
        }
        return ControlLine{};
    }

private:
    std::unique_ptr<SocketStream> stream_;
    std::unique_ptr<FrameReader> reader_;
};

std::string HashOf(const std::vector<uint8_t> &pcm) {
    return bridge::FormatHash(bridge::Xxh64(pcm.data(), pcm.size()));
}

/// `"port":<n>` out of an advertisement, without a JSON parser: nlohmann is private to
/// messages.cpp, and a test that reads the file the way a stranger would is the point here.
int PortIn(const std::string &json) {
    size_t at = json.find("\"port\":");
    if (at == std::string::npos) {
        return 0;
    }
    return std::atoi(json.c_str() + at + 7);
}

std::string LayoutOf(int trackNo, double startMs, const std::string &hash) {
    return R"({"trackNo":)" + std::to_string(trackNo) + R"(,"startMs":)" +
           std::to_string(startMs) + R"(,"endMs":)" + std::to_string(startMs + 1000.0) +
           R"(,"audioHash":")" + hash + R"("})";
}

/// One block of output.
struct Block {
    std::vector<float> left;
    std::vector<float> right;

    explicit Block(size_t frames) : left(frames, 0.0f), right(frames, 0.0f) {}

    void Render(Session &session, int64_t fromFrame) {
        session.Render(fromFrame, left.size(), left.data(), right.data());
    }
};

/// The worker publishes a snapshot on its own schedule, so a render is retried until the block
/// looks the way the test is waiting for. False if it never does, which is a failure and not a
/// slow machine.
template <typename Ready>
bool RenderUntil(Session &session, int64_t fromFrame, Block *block, Ready ready) {
    int64_t deadline = bridge::NowMs() + kWaitMs;
    while (bridge::NowMs() < deadline) {
        block->Render(session, fromFrame);
        if (ready(*block)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

/// Polls a condition the worker thread is responsible for. False if it never held, which is a
/// failure rather than a slow machine.
template <typename Ready>
bool WaitUntil(Ready ready) {
    int64_t deadline = bridge::NowMs() + kWaitMs;
    while (bridge::NowMs() < deadline) {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool WaitUntilDisconnected(const Session &session) {
    return WaitUntil([&session] { return !session.IsConnected(); });
}
}  // namespace

TEST_CASE("A session is found through its advertisement and plays what OpenUtau sends it") {
    // The milestone criterion with the real plumbing behind it: discovery file, socket, protocol,
    // store, snapshot and mixer, driven by a client that is told nothing it could not read itself.
    TempDir dir;
    std::vector<float> left = {0.25f, -0.5f, 0.75f, -1.0f};
    std::vector<float> right = {-0.25f, 0.5f, -0.75f, 1.0f};
    std::vector<uint8_t> pcm = WirePcm(left, right);
    std::string hash = HashOf(pcm);

    Session session(dir.path);
    // The wire rate, so what comes out is what went in rather than a resampling of it.
    session.SetHostSampleRate(44100.0);
    session.SetTrackNo(0);
    REQUIRE(session.Start());
    REQUIRE(session.Port() != 0);

    std::vector<fs::path> advertised = FilesIn(dir.path);
    REQUIRE(advertised.size() == 1);
    int port = PortIn(ReadAll(advertised[0]));
    CHECK(port == session.Port());

    FakeOpenUtau utau;
    REQUIRE(utau.Connect(port));

    REQUIRE(utau.Send(
        bridge::BuildRequestLine("c-1", kind::kInit, R"({"ustx":"name: project"})")));
    ControlLine ack = utau.ReadControl();
    REQUIRE(ack.kind == ControlKind::Response);
    CHECK(ack.uuid == "c-1");

    // The bridge sends the rendered part unchanged; the DAW owns gain and pan.
    REQUIRE(utau.Send(bridge::BuildNotificationLine(
        kind::kUpdateTracks,
        R"({"tracks":[{"name":"Lead","volume":-24,"pan":100,"muted":true}]})")));

    REQUIRE(utau.Send(bridge::BuildRequestLine("c-2", kind::kUpdatePartLayout,
                                              R"({"parts":[)" + LayoutOf(0, 1000.0, hash) +
                                                  R"(]})")));
    ControlLine layoutAck = utau.ReadControl();
    REQUIRE(layoutAck.kind == ControlKind::Response);
    CHECK(layoutAck.payload.find(hash) != std::string::npos);  // Nothing is held yet.

    ControlLine pull = utau.ReadControl();
    REQUIRE(pull.kind == ControlKind::Request);
    CHECK(pull.name == kind::kGetAudio);
    CHECK(pull.payload.find(hash) != std::string::npos);
    REQUIRE(utau.SendFrame(hash, pcm));

    // The part plays one second in, which at the wire rate is frame 44100.
    Block block(4);
    REQUIRE(RenderUntil(session, 44100, &block,
                        [](const Block &out) { return out.left[0] != 0.0f; }));
    CHECK(block.left == left);
    CHECK(block.right == right);

    Block elsewhere(4);
    elsewhere.Render(session, 0);
    CHECK(elsewhere.left == std::vector<float>(4, 0.0f));

    // The transport, which is the only thing the plugin brings up unprompted.
    session.NoteTransport(false, 0.0, false, false, 0.0);
    session.NoteTransport(true, 1.0, true, false, 0.0);
    ControlLine started = utau.ReadControl();
    CHECK(started.kind == ControlKind::Notification);
    CHECK(started.name == kind::kPlaybackStarted);
    // Still playing is not a start: an edge is a change, not a state. The playhead now
    // streams while playing, so everything but a second start is tolerated here.
    session.NoteTransport(true, 1.01, true, false, 0.0);
    bool anotherStart = false;
    int64_t edgeDeadline = bridge::NowMs() + 400;
    while (bridge::NowMs() < edgeDeadline) {
        ControlLine line = utau.ReadControl(50);
        if (line.kind == ControlKind::Notification && line.name == kind::kPlaybackStarted) {
            anotherStart = true;
            break;
        }
    }
    CHECK_FALSE(anotherStart);

    REQUIRE(utau.Send("close"));
    CHECK(WaitUntilDisconnected(session));
    // The audio outlives the connection on purpose: a project whose editor has been closed is
    // still a project the DAW can play and mix.
    Block afterwards(4);
    afterwards.Render(session, 44100);
    CHECK(afterwards.left == left);

    session.Stop();
    CHECK(FilesIn(dir.path).empty());  // An instance that is gone must not be found.
}

TEST_CASE("Retargeting the instance to another track changes what it plays") {
    TempDir dir;
    std::vector<uint8_t> mine = WirePcm({0.5f, 0.5f}, {0.5f, 0.5f});
    std::vector<uint8_t> theirs = WirePcm({-0.25f, -0.25f}, {-0.25f, -0.25f});
    std::string mineHash = HashOf(mine);
    std::string theirsHash = HashOf(theirs);

    Session session(dir.path);
    session.SetHostSampleRate(44100.0);
    REQUIRE(session.Start());
    FakeOpenUtau utau;
    REQUIRE(utau.Connect(session.Port()));

    // No `init` here: the plugin does not gate a layout on it, and this case is about the track
    // index rather than the handshake.
    REQUIRE(utau.Send(bridge::BuildNotificationLine(
        kind::kUpdateTracks,
        R"({"tracks":[{"name":"A","volume":0,"pan":-100},)"
        R"({"name":"B","volume":0,"pan":-100}]})")));
    REQUIRE(utau.Send(bridge::BuildRequestLine(
        "c-1", kind::kUpdatePartLayout,
        R"({"parts":[)" + LayoutOf(0, 0.0, mineHash) + "," + LayoutOf(1, 0.0, theirsHash) +
            R"(]})")));
    ControlLine ack = utau.ReadControl();
    REQUIRE(ack.kind == ControlKind::Response);

    // Both parts are missing and §6.2 allows one pull at a time, so the queue drains in order.
    for (int i = 0; i < 2; i++) {
        ControlLine pull = utau.ReadControl();
        REQUIRE(pull.name == kind::kGetAudio);
        if (pull.payload.find(mineHash) != std::string::npos) {
            REQUIRE(utau.SendFrame(mineHash, mine));
        } else {
            REQUIRE(pull.payload.find(theirsHash) != std::string::npos);
            REQUIRE(utau.SendFrame(theirsHash, theirs));
        }
    }

    Block first(2);
    REQUIRE(RenderUntil(session, 0, &first,
                        [](const Block &out) { return out.left[0] == 0.5f; }));

    session.SetTrackNo(1);
    // The same layout at the same position: only which track this instance answers for changed,
    // and the other track's part was already held, so nothing is pulled again.
    Block second(2);
    REQUIRE(RenderUntil(session, 0, &second,
                        [](const Block &out) { return out.left[0] == -0.25f; }));
    CHECK(second.left == std::vector<float>{-0.25f, -0.25f});

    session.Stop();
}

TEST_CASE("A host sample-rate change makes the session ask for every part again") {
    // Clips are converted on arrival, so a rate change invalidates all of them at once, and §6.2
    // has no message for asking OpenUtau to re-send a layout: the hashes are the whole request.
    TempDir dir;
    std::vector<float> flat(512, 0.5f);
    std::vector<uint8_t> pcm = WirePcm(flat, flat);
    std::string hash = HashOf(pcm);

    Session session(dir.path);
    session.SetHostSampleRate(44100.0);
    REQUIRE(session.Start());
    FakeOpenUtau utau;
    REQUIRE(utau.Connect(session.Port()));

    REQUIRE(utau.Send(bridge::BuildNotificationLine(
        kind::kUpdateTracks, R"({"tracks":[{"name":"Lead","volume":0,"pan":-100}]})")));
    REQUIRE(utau.Send(bridge::BuildRequestLine("c-1", kind::kUpdatePartLayout,
                                              R"({"parts":[)" + LayoutOf(0, 0.0, hash) +
                                                  R"(]})")));
    REQUIRE(utau.ReadControl().kind == ControlKind::Response);
    REQUIRE(utau.ReadControl().name == kind::kGetAudio);
    REQUIRE(utau.SendFrame(hash, pcm));

    Block atWireRate(4);
    REQUIRE(RenderUntil(session, 0, &atWireRate,
                        [](const Block &out) { return out.left[3] == 0.5f; }));

    session.SetHostSampleRate(48000.0);
    ControlLine again = utau.ReadControl();
    CHECK(again.name == kind::kGetAudio);
    CHECK(again.payload.find(hash) != std::string::npos);
    REQUIRE(utau.SendFrame(hash, pcm));

    // Converted this time, so the level is what is checked, and past the resampler's settling.
    Block atHostRate(256);
    REQUIRE(RenderUntil(session, 0, &atHostRate,
                        [](const Block &out) { return out.left[200] > 0.4f; }));
    CHECK(atHostRate.left[200] == doctest::Approx(0.5).epsilon(0.02));
    CHECK(atHostRate.right[200] == doctest::Approx(0.5).epsilon(0.02));

    session.Stop();
}

TEST_CASE("The transport is reported upstream and project info reaches the window") {
    // v1.1: the playhead and bpm notifications, throttled by the worker, and the
    // updateProjectInfo snapshot the info window reads.
    TempDir dir;
    Session session(dir.path);
    session.SetHostSampleRate(44100.0);
    REQUIRE(session.Start());
    FakeOpenUtau utau;
    REQUIRE(utau.Connect(session.Port()));

    // Reads past anything that is not the wanted notification (heartbeats, playbackStarted,
    // and the playhead's own repeats while playing).
    auto ReadUntil = [&](const char *kindName, const std::string &needle) {
        int64_t deadline = bridge::NowMs() + kWaitMs;
        while (bridge::NowMs() < deadline) {
            ControlLine line = utau.ReadControl(100);
            if (line.kind == ControlKind::Notification && line.name == kindName &&
                (needle.empty() || line.payload.find(needle) != std::string::npos)) {
                return line;
            }
        }
        return ControlLine{};
    };

    // Project info is window state, not mixer state: it changes what UiCopy shows.
    REQUIRE(utau.Send(bridge::BuildNotificationLine(
        kind::kUpdateProjectInfo, R"({"name":"my song","saved":true})")));
    REQUIRE(WaitUntil([&session] {
        bridge::UiState state = session.UiCopy();
        return state.projectName == "my song" && state.projectSaved;
    }));

    // A parked transport with a position: the first report is a state change, and the tempo
    // has never been sent.
    session.NoteTransport(true, 2.0, false, true, 120.0);
    ControlLine playhead = ReadUntil(kind::kPlayhead, "\"positionMs\":2000");
    CHECK(playhead.payload.find("\"playing\":false") != std::string::npos);
    ControlLine bpm = ReadUntil(kind::kBpm, "120");
    CHECK(bpm.name == kind::kBpm);

    // Parked at the same place is not news.
    session.NoteTransport(true, 2.0, false, true, 120.0);
    CHECK(utau.ReadControl(300).kind == ControlKind::Unknown);

    // A scrub is.
    session.NoteTransport(true, 5.0, false, true, 120.0);
    ReadUntil(kind::kPlayhead, "\"positionMs\":5000");

    // Playing reports continuously, at the throttle's pace rather than on every poll.
    session.NoteTransport(true, 5.0, true, true, 120.0);
    playhead = ReadUntil(kind::kPlayhead, "\"playing\":true");
    bool saw6000 = WaitUntil([&] {
        session.NoteTransport(true, 6.0, true, true, 120.0);
        ControlLine line = utau.ReadControl(50);
        return line.kind == ControlKind::Notification && line.name == kind::kPlayhead &&
               line.payload.find("\"positionMs\":6000") != std::string::npos;
    });
    CHECK(saw6000);

    // Tempo changes are reported; sub-epsilon ones are not.
    session.NoteTransport(true, 6.0, true, true, 137.5);
    ReadUntil(kind::kBpm, "137.5");
    session.NoteTransport(true, 6.0, false, true, 137.505);  // Stops, to silence the playhead.
    ReadUntil(kind::kPlayhead, "");                          // The stop itself is a change.
    session.NoteTransport(true, 6.0, false, true, 137.505);
    CHECK(utau.ReadControl(300).kind == ControlKind::Unknown);

    session.Stop();
}

TEST_CASE("A bounce waits for audio a playback would have rendered as silence") {
    // The difference between the two modes is the whole point: real time cannot wait, and a
    // bounce runs through the project far faster than OpenUtau can send it, so it must.
    TempDir dir;
    std::vector<uint8_t> pcm = WirePcm({0.5f, 0.5f}, {0.5f, 0.5f});
    std::string hash = HashOf(pcm);

    Session session(dir.path);
    session.SetHostSampleRate(44100.0);
    REQUIRE(session.Start());
    FakeOpenUtau utau;
    REQUIRE(utau.Connect(session.Port()));

    REQUIRE(utau.Send(bridge::BuildNotificationLine(
        kind::kUpdateTracks, R"({"tracks":[{"name":"Lead","volume":0,"pan":-100}]})")));
    REQUIRE(utau.Send(bridge::BuildRequestLine("c-1", kind::kUpdatePartLayout,
                                              R"({"parts":[)" + LayoutOf(0, 0.0, hash) +
                                                  R"(]})")));
    REQUIRE(utau.ReadControl().kind == ControlKind::Response);
    REQUIRE(utau.ReadControl().name == kind::kGetAudio);
    // The frame is deliberately withheld: this is the state a bounce must not render through.
    REQUIRE(WaitUntil([&session] { return !session.IsSynced(); }));

    SUBCASE("in real time the block is silent immediately") {
        int64_t before = bridge::NowMs();
        Block live(2);
        live.Render(session, 0);
        CHECK(bridge::NowMs() - before < 1000);  // Nothing like the offline budget.
        CHECK(live.left == std::vector<float>(2, 0.0f));
    }

    SUBCASE("offline it waits, and then renders what arrived") {
        session.SetOffline(true);
        std::thread late([&utau, &hash, &pcm] {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            utau.SendFrame(hash, pcm);
        });

        Block bounced(2);
        bounced.Render(session, 0);
        late.join();

        CHECK(session.IsSynced());
        CHECK(bounced.left == std::vector<float>(2, 0.5f));
    }

    SUBCASE("offline with nobody connected does not wait at all") {
        // Waiting for a part that no longer has anywhere to come from would stall a bounce for
        // its whole budget on every block.
        REQUIRE(utau.Send("close"));
        REQUIRE(WaitUntilDisconnected(session));
        session.SetOffline(true);

        int64_t before = bridge::NowMs();
        Block bounced(2);
        bounced.Render(session, 0);
        CHECK(bridge::NowMs() - before < 1000);
        CHECK(bounced.left == std::vector<float>(2, 0.0f));
    }

    session.Stop();
}

TEST_CASE("Track metadata and the picker's request path reach the window") {
    // v1.2: updateTracks carries singer/engine per track, the window reads them out of
    // UiCopy, and a pick made in the window changes the routing immediately while leaving
    // exactly one pending request for the plugin to report to the host.
    TempDir dir;
    Session session(dir.path);
    session.SetHostSampleRate(44100.0);
    REQUIRE(session.Start());
    FakeOpenUtau utau;
    REQUIRE(utau.Connect(session.Port()));

    REQUIRE(utau.Send(bridge::BuildNotificationLine(
        kind::kUpdateTracks,
        R"({"tracks":[{"name":"Lead","singer":"Kikyo","engine":"DIFFSINGER"},)"
        R"({"name":"Harmony"}]})")));
    REQUIRE(WaitUntil([&session] {
        bridge::UiState state = session.UiCopy();
        return state.tracks.size() == 2 && state.tracks[0].singer == "Kikyo" &&
               state.tracks[0].engine == "DIFFSINGER" && state.tracks[1].singer.empty();
    }));

    // The host set the track: no pending request may exist, so nothing would be echoed.
    session.SetTrackNo(1);
    CHECK_FALSE(session.ConsumeTrackRequest());

    // The picker set the track: the routing follows at once, and the plugin consumes the
    // request exactly once before it goes quiet again.
    session.RequestTrackNo(0);
    CHECK(session.TrackNo() == 0);
    CHECK(session.ConsumeTrackRequest());
    CHECK_FALSE(session.ConsumeTrackRequest());

    session.Stop();
}
