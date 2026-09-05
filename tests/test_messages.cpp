#include <doctest.h>

#include "messages.h"

#include <string>
#include <vector>

using bridge::BuildDiscoveryJson;
using bridge::BuildBpmPayload;
using bridge::BuildFailEnvelope;
using bridge::BuildGetAudioPayload;
using bridge::BuildInitResponseEnvelope;
using bridge::BuildPartLayoutResponseEnvelope;
using bridge::BuildPlayheadPayload;
using bridge::Envelope;
using bridge::ParseEnvelope;
using bridge::ParsePartLayoutRequest;
using bridge::ParseProjectInfoNotification;
using bridge::ParseTracksNotification;
using bridge::ParseUstxPayload;
using bridge::PartLayout;
using bridge::ProjectInfo;
using bridge::TrackInfo;

namespace {
bool Contains(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}
}  // namespace

TEST_CASE("the ustx payload is read as an opaque document") {
    std::string ustx;
    REQUIRE(ParseUstxPayload("{\"ustx\":\"name: test\\ntracks: []\\n\"}", &ustx));
    // USTX is YAML, so the plugin must keep it verbatim rather than reinterpret it.
    CHECK(ustx == "name: test\ntracks: []\n");
}

TEST_CASE("a ustx payload of the wrong shape is refused") {
    std::string ustx;
    CHECK_FALSE(ParseUstxPayload("{}", &ustx));
    CHECK_FALSE(ParseUstxPayload("{\"ustx\":null}", &ustx));
    CHECK_FALSE(ParseUstxPayload("{\"ustx\":42}", &ustx));
    CHECK_FALSE(ParseUstxPayload("not json", &ustx));
    CHECK_FALSE(ParseUstxPayload("[]", &ustx));
}

TEST_CASE("part layouts parse with their hash kept as a decimal string") {
    std::vector<PartLayout> parts;
    REQUIRE(ParsePartLayoutRequest(
        "{\"parts\":[{\"trackNo\":0,\"startMs\":1200.0,\"endMs\":8400.0,"
        "\"audioHash\":\"13507256038857166760\"}]}",
        &parts));
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].trackNo == 0);
    CHECK(parts[0].startMs == doctest::Approx(1200.0));
    CHECK(parts[0].endMs == doctest::Approx(8400.0));
    CHECK(parts[0].audioHash == "13507256038857166760");
}

TEST_CASE("an empty layout is valid — it means the project has no rendered parts") {
    std::vector<PartLayout> parts{PartLayout{}};
    REQUIRE(ParsePartLayoutRequest("{\"parts\":[]}", &parts));
    CHECK(parts.empty());
}

TEST_CASE("unknown fields are ignored, as append-only versioning requires") {
    std::vector<PartLayout> parts;
    REQUIRE(ParsePartLayoutRequest(
        "{\"parts\":[{\"trackNo\":2,\"startMs\":0,\"endMs\":1,\"audioHash\":\"7\","
        "\"fadeMs\":5}],\"tempoMap\":[]}",
        &parts));
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].trackNo == 2);
}

TEST_CASE("a layout entry whose hash is not a decimal integer is refused") {
    std::vector<PartLayout> parts;
    // Not merely defaulted: an unpullable, undedupable hash would strand the part forever.
    CHECK_FALSE(ParsePartLayoutRequest(
        "{\"parts\":[{\"audioHash\":\"0x1f\",\"startMs\":0,\"endMs\":1}]}", &parts));
    CHECK_FALSE(ParsePartLayoutRequest("{\"parts\":[{\"startMs\":0,\"endMs\":1}]}", &parts));
    CHECK_FALSE(ParsePartLayoutRequest(
        "{\"parts\":[{\"audioHash\":13507256038857166760}]}", &parts));
    CHECK_FALSE(ParsePartLayoutRequest("{\"parts\":{}}", &parts));
}

TEST_CASE("tracks parse in OpenUtau's own scale") {
    std::vector<TrackInfo> tracks;
    REQUIRE(ParseTracksNotification(
        "{\"tracks\":[{\"name\":\"Lead\",\"volume\":-3.0,\"pan\":-20.0,\"muted\":false,"
        "\"singer\":\"Kikyo\",\"engine\":\"DIFFSINGER\"},"
        "{\"name\":\"Harmony\",\"volume\":0.0,\"pan\":15.0,\"muted\":true}]}",
        &tracks));
    REQUIRE(tracks.size() == 2);
    CHECK(tracks[0].name == "Lead");
    CHECK(tracks[0].volume == doctest::Approx(-3.0));
    CHECK(tracks[0].pan == doctest::Approx(-20.0));
    CHECK_FALSE(tracks[0].muted);
    CHECK(tracks[0].singer == "Kikyo");
    CHECK(tracks[0].engine == "DIFFSINGER");
    CHECK(tracks[1].muted);
}

TEST_CASE("a track that omits muted is audible") {
    std::vector<TrackInfo> tracks;
    REQUIRE(ParseTracksNotification("{\"tracks\":[{\"name\":\"Lead\"}]}", &tracks));
    REQUIRE(tracks.size() == 1);
    CHECK_FALSE(tracks[0].muted);
    CHECK(tracks[0].volume == doctest::Approx(0.0));
}

TEST_CASE("v1.2 informational fields default to empty, not to noise") {
    // A 1.1 peer never sends singer/engine; null must behave like absent, which is what
    // OpenUtau sends for a track with no singer assigned yet.
    std::vector<TrackInfo> tracks;
    REQUIRE(ParseTracksNotification(
        "{\"tracks\":[{\"name\":\"Lead\",\"singer\":null,\"engine\":null}]}", &tracks));
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0].singer.empty());
    CHECK(tracks[0].engine.empty());
}

TEST_CASE("envelopes report success and carry their data") {
    Envelope envelope;
    std::string initResponse = BuildInitResponseEnvelope();
    REQUIRE(ParseEnvelope(initResponse, &envelope));
    CHECK(envelope.success);
    CHECK(envelope.error.empty());
    CHECK(Contains(initResponse, "\"apiVersion\":\"1.2\""));

    std::string layoutResponse = BuildPartLayoutResponseEnvelope({"7", "8"});
    REQUIRE(ParseEnvelope(layoutResponse, &envelope));
    CHECK(envelope.success);
    CHECK(Contains(layoutResponse, "\"missingAudios\":[\"7\",\"8\"]"));

    std::string empty = BuildPartLayoutResponseEnvelope({});
    CHECK(Contains(empty, "\"missingAudios\":[]"));
}

TEST_CASE("a failure envelope carries its reason") {
    Envelope envelope;
    REQUIRE(ParseEnvelope(BuildFailEnvelope("no such hash"), &envelope));
    CHECK_FALSE(envelope.success);
    CHECK(envelope.error == "no such hash");
}

TEST_CASE("an envelope without a boolean success field is not an envelope") {
    Envelope envelope;
    CHECK_FALSE(ParseEnvelope("{}", &envelope));
    CHECK_FALSE(ParseEnvelope("{\"success\":\"true\"}", &envelope));
    CHECK_FALSE(ParseEnvelope("nonsense", &envelope));
}

TEST_CASE("getAudio sends its hash as a string") {
    // A JSON number here would round-trip wrong past 2^53 (§5.2).
    CHECK(BuildGetAudioPayload("13507256038857166760") ==
          "{\"hash\":\"13507256038857166760\"}");
}

TEST_CASE("the discovery file advertises port, name and version") {
    std::string json = BuildDiscoveryJson(52341, "OpenUtau Bridge (Track 1)");
    CHECK(Contains(json, "\"port\":52341"));
    CHECK(Contains(json, "\"name\":\"OpenUtau Bridge (Track 1)\""));
    CHECK(Contains(json, "\"apiVersion\":\"1.2\""));
}

TEST_CASE("project info parses its name and saved state") {
    ProjectInfo info;
    REQUIRE(ParseProjectInfoNotification(R"({"name":"my song","saved":true})", &info));
    CHECK(info.name == "my song");
    CHECK(info.saved);

    // An unsaved project reports no name and saved false, on the wire as absent fields.
    REQUIRE(ParseProjectInfoNotification("{}", &info));
    CHECK(info.name.empty());
    CHECK_FALSE(info.saved);

    // Wrong-typed fields default rather than throw; only a non-object is a shape error.
    REQUIRE(ParseProjectInfoNotification(R"({"name":42,"saved":"yes"})", &info));
    CHECK(info.name.empty());
    CHECK_FALSE(info.saved);
    CHECK_FALSE(ParseProjectInfoNotification("nonsense", &info));
    CHECK_FALSE(ParseProjectInfoNotification("[]", &info));
}

TEST_CASE("the playhead payload names its position and play state") {
    std::string json = BuildPlayheadPayload(1250.0, true);
    CHECK(Contains(json, "\"positionMs\":1250"));
    CHECK(Contains(json, "\"playing\":true"));

    CHECK(Contains(BuildPlayheadPayload(0.0, false), "\"playing\":false"));
}

TEST_CASE("the bpm payload carries the tempo") {
    CHECK(BuildBpmPayload(120.0) == "{\"bpm\":120.0}");
    CHECK(BuildBpmPayload(137.5) == "{\"bpm\":137.5}");
}
