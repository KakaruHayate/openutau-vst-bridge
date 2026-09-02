#include "connection.h"

#include "frame.h"
#include "hash.h"
#include "loopback.h"
#include "messages.h"
#include "reader.h"

#include <doctest.h>

#include <string>
#include <vector>

using bridge::Connection;
using bridge::ConnectionEnd;
using bridge::ControlKind;
using bridge::ControlLine;
using bridge::Envelope;
using bridge::FrameReader;
using bridge::ReadStatus;
using bridge::test::CollectingHandler;
using bridge::test::kWaitMs;
using bridge::test::Loopback;
using bridge::test::Pcm;
using bridge::test::WriteLine;

namespace kind = bridge::kind;

TEST_CASE("init, updatePartLayout, getAudio and playbackStarted over a real socket") {
    // The completion criterion for this milestone, driven end to end: the test plays OpenUtau,
    // the Connection is the plugin, and nothing between them is simulated.
    Loopback loop;
    REQUIRE(loop.Open());

    CollectingHandler handler;
    Connection connection(loop.server.get(), &handler);
    FrameReader client(loop.client.get());
    std::string line;

    std::vector<uint8_t> pcm = Pcm(1024);
    std::string hash = bridge::FormatHash(bridge::Xxh64(pcm.data(), pcm.size()));
    handler.missing = {hash};

    // 1. The baseline document.
    REQUIRE(WriteLine(loop.client.get(),
                      bridge::BuildRequestLine("c-1", kind::kInit,
                                               R"({"ustx":"name: project"})")));
    REQUIRE(connection.Poll(kWaitMs));
    CHECK(connection.IsInitialized());
    REQUIRE(handler.ustx.size() == 1);
    CHECK(handler.ustx[0] == "name: project");

    REQUIRE(client.ReadLine(&line, kWaitMs) == ReadStatus::Ok);
    ControlLine initResponse = bridge::ParseControlLine(line);
    REQUIRE(initResponse.kind == ControlKind::Response);
    CHECK(initResponse.uuid == "c-1");
    Envelope envelope;
    REQUIRE(bridge::ParseEnvelope(initResponse.payload, &envelope));
    CHECK(envelope.success);

    // 2. The part layout, whose answer lists what the plugin does not hold.
    std::string layout =
        R"({"parts":[{"trackNo":0,"startMs":0,"endMs":2000,"audioHash":")" + hash + R"("}]})";
    REQUIRE(WriteLine(loop.client.get(),
                      bridge::BuildRequestLine("c-2", kind::kUpdatePartLayout, layout)));
    REQUIRE(connection.Poll(kWaitMs));
    REQUIRE(handler.parts.size() == 1);
    CHECK(handler.parts[0].trackNo == 0);
    CHECK(handler.parts[0].endMs == doctest::Approx(2000.0));
    CHECK(handler.parts[0].audioHash == hash);

    REQUIRE(client.ReadLine(&line, kWaitMs) == ReadStatus::Ok);
    ControlLine layoutResponse = bridge::ParseControlLine(line);
    REQUIRE(layoutResponse.kind == ControlKind::Response);
    CHECK(layoutResponse.uuid == "c-2");
    CHECK(layoutResponse.payload.find(hash) != std::string::npos);

    // 3. The pull, answered on the data plane and correlated by hash rather than by uuid.
    REQUIRE(client.ReadLine(&line, kWaitMs) == ReadStatus::Ok);
    ControlLine pull = bridge::ParseControlLine(line);
    REQUIRE(pull.kind == ControlKind::Request);
    CHECK(pull.name == kind::kGetAudio);
    CHECK(pull.payload.find(hash) != std::string::npos);

    std::string header = bridge::BuildFrameHeader(hash, pcm.size());
    REQUIRE(loop.client->Write(header.data(), header.size()));
    REQUIRE(loop.client->Write(pcm.data(), pcm.size()));
    REQUIRE(connection.Poll(kWaitMs));
    REQUIRE(handler.audio.size() == 1);
    CHECK(handler.audio[0].first == hash);
    CHECK(handler.audio[0].second == pcm);

    // 4. The plugin reports the transport, which is the only thing it initiates unprompted.
    REQUIRE(connection.SendNotification(kind::kPlaybackStarted, bridge::BuildEmptyPayload()));
    REQUIRE(client.ReadLine(&line, kWaitMs) == ReadStatus::Ok);
    ControlLine started = bridge::ParseControlLine(line);
    CHECK(started.kind == ControlKind::Notification);
    CHECK(started.name == kind::kPlaybackStarted);

    // 5. OpenUtau ends the session; only that side sends close (§5.1).
    REQUIRE(WriteLine(loop.client.get(), "close"));
    CHECK(!connection.Poll(kWaitMs));
    CHECK(connection.End() == ConnectionEnd::PeerClosed);
}

TEST_CASE("A second layout re-lists an abandoned hash, which is the retry") {
    // Nothing in the protocol re-sends a dropped pull. The next updatePartLayout naming the
    // hash is what asks again, so a refusal must leave the connection able to serve one.
    Loopback loop;
    REQUIRE(loop.Open());

    CollectingHandler handler;
    Connection connection(loop.server.get(), &handler);
    FrameReader client(loop.client.get());
    std::string line;

    std::vector<uint8_t> pcm = Pcm(8, 200);
    std::string hash = bridge::FormatHash(bridge::Xxh64(pcm.data(), pcm.size()));
    handler.missing = {hash};
    std::string layout =
        R"({"parts":[{"trackNo":0,"startMs":0,"endMs":100,"audioHash":")" + hash + R"("}]})";

    REQUIRE(WriteLine(loop.client.get(),
                      bridge::BuildRequestLine("c-1", kind::kUpdatePartLayout, layout)));
    REQUIRE(connection.Poll(kWaitMs));
    REQUIRE(client.ReadLine(&line, kWaitMs) == ReadStatus::Ok);  // The layout answer.
    REQUIRE(client.ReadLine(&line, kWaitMs) == ReadStatus::Ok);  // The pull.
    ControlLine pull = bridge::ParseControlLine(line);
    REQUIRE(pull.name == kind::kGetAudio);

    // Refused, e.g. the part was deleted between the layout and the pull.
    REQUIRE(WriteLine(loop.client.get(),
                      bridge::BuildResponseLine(pull.uuid,
                                                R"({"success":false,"error":"unknown hash"})")));
    REQUIRE(connection.Poll(kWaitMs));
    CHECK(handler.audio.empty());
    CHECK(connection.End() == ConnectionEnd::Running);

    // The same layout again, and the plugin asks again.
    REQUIRE(WriteLine(loop.client.get(),
                      bridge::BuildRequestLine("c-2", kind::kUpdatePartLayout, layout)));
    REQUIRE(connection.Poll(kWaitMs));
    REQUIRE(client.ReadLine(&line, kWaitMs) == ReadStatus::Ok);  // The second layout answer.
    REQUIRE(client.ReadLine(&line, kWaitMs) == ReadStatus::Ok);
    ControlLine retry = bridge::ParseControlLine(line);
    CHECK(retry.name == kind::kGetAudio);
    CHECK(retry.uuid != pull.uuid);
    CHECK(retry.payload.find(hash) != std::string::npos);

    std::string header = bridge::BuildFrameHeader(hash, pcm.size());
    REQUIRE(loop.client->Write(header.data(), header.size()));
    REQUIRE(loop.client->Write(pcm.data(), pcm.size()));
    REQUIRE(connection.Poll(kWaitMs));
    REQUIRE(handler.audio.size() == 1);
    CHECK(handler.audio[0].second == pcm);
}
