#include "socket.h"

#include "frame.h"
#include "loopback.h"
#include "reader.h"

#include <doctest.h>

#include <string>
#include <vector>

using bridge::FrameReader;
using bridge::Listener;
using bridge::ReadStatus;
using bridge::test::kWaitMs;
using bridge::test::Loopback;
using bridge::test::Pcm;
using bridge::test::WriteLine;

TEST_CASE("The listener binds a loopback port and accepts one client") {
    Loopback loop;
    REQUIRE(loop.Open());
    CHECK(loop.listener.Port() > 0);
    CHECK(loop.server->IsOpen());

    SUBCASE("control lines cross in both directions") {
        REQUIRE(WriteLine(loop.client.get(), "notification:ping {}"));
        FrameReader serverReader(loop.server.get());
        std::string line;
        REQUIRE(serverReader.ReadLine(&line, kWaitMs) == ReadStatus::Ok);
        CHECK(line == "notification:ping {}");

        REQUIRE(WriteLine(loop.server.get(), "response:u-1 {}"));
        FrameReader clientReader(loop.client.get());
        REQUIRE(clientReader.ReadLine(&line, kWaitMs) == ReadStatus::Ok);
        CHECK(line == "response:u-1 {}");
    }

    SUBCASE("a binary frame survives being written in pieces") {
        std::vector<uint8_t> pcm = Pcm(512);
        std::string header = bridge::BuildFrameHeader("42", pcm.size());
        REQUIRE(loop.client->Write(header.data(), header.size()));
        // Split mid-payload: a socket is free to deliver it this way regardless.
        REQUIRE(loop.client->Write(pcm.data(), 7));
        REQUIRE(loop.client->Write(pcm.data() + 7, pcm.size() - 7));

        FrameReader reader(loop.server.get());
        std::string line;
        REQUIRE(reader.ReadLine(&line, kWaitMs) == ReadStatus::Ok);
        std::string hash;
        int32_t length = 0;
        REQUIRE(bridge::TryParseFrameHeader(line, &hash, &length));
        CHECK(hash == "42");
        REQUIRE(length == static_cast<int32_t>(pcm.size()));
        std::vector<uint8_t> received;
        REQUIRE(reader.ReadExactly(static_cast<size_t>(length), &received) == ReadStatus::Ok);
        CHECK(received == pcm);
    }

    SUBCASE("a closed client is an end of stream, not an error") {
        loop.client->Close();
        FrameReader reader(loop.server.get());
        std::string line;
        CHECK(reader.ReadLine(&line, kWaitMs) == ReadStatus::EndOfStream);
    }
}

TEST_CASE("Accept gives up rather than blocking forever") {
    Listener listener;
    REQUIRE(listener.Start());
    CHECK(listener.Accept(10) == nullptr);
    listener.Stop();
    CHECK(listener.Port() == 0);
    CHECK(listener.Accept(10) == nullptr);
}
