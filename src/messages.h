#pragma once

/*
 * The JSON payloads of PROTOCOL.md §6, mirroring `DawMessages.cs`. nlohmann/json stays
 * inside messages.cpp: it is a very large header and nothing else needs it.
 *
 * Parsers tolerate unknown and missing fields (§10) and never throw; they return false only
 * when the payload is not the shape its kind requires.
 */

#include <string>
#include <vector>

namespace bridge {

/// Control-plane kinds. Mirrors `DawMessageKind`.
namespace kind {
inline constexpr const char *kInit = "init";
inline constexpr const char *kUpdateUstx = "updateUstx";
inline constexpr const char *kUpdatePartLayout = "updatePartLayout";
inline constexpr const char *kGetAudio = "getAudio";
inline constexpr const char *kUpdateTracks = "updateTracks";
inline constexpr const char *kPing = "ping";
inline constexpr const char *kPlaybackStarted = "playbackStarted";
}  // namespace kind

/// One entry of `updatePartLayout`. Windows are absolute milliseconds on OpenUtau's
/// timeline, not the DAW's — a differing tempo misaligns musically until tempo sync lands.
struct PartLayout {
    int trackNo = 0;
    double startMs = 0.0;
    double endMs = 0.0;
    std::string audioHash;
};

/// One entry of `updateTracks`, in OpenUtau's internal scale (§6.1).
struct TrackInfo {
    std::string name;
    /// Decibels; 0 is unity. Convert with the fader law in fader.h, not 10^(dB/20).
    double volume = 0.0;
    /// -100..+100; 0 is centre.
    double pan = 0.0;
    /// The effective mute, with solo already resolved against the rest of the project.
    bool muted = false;
};

/// A `response:<uuid>` envelope, as read when a peer refuses one of our requests.
struct Envelope {
    bool success = false;
    std::string error;
};

/// `{ "ustx": "<document>" }`, shared by `init` and `updateUstx`.
bool ParseUstxPayload(const std::string &json, std::string *ustx);
bool ParsePartLayoutRequest(const std::string &json, std::vector<PartLayout> *parts);
bool ParseTracksNotification(const std::string &json, std::vector<TrackInfo> *tracks);
bool ParseEnvelope(const std::string &json, Envelope *envelope);

/// Complete `{ "success": …, "data": …, "error": … }` envelopes, ready to send.
std::string BuildInitResponseEnvelope();
std::string BuildPartLayoutResponseEnvelope(const std::vector<std::string> &missingAudios);
std::string BuildFailEnvelope(const std::string &error);

std::string BuildGetAudioPayload(const std::string &hash);

/// `{}` — the payload of the kinds that carry no fields.
std::string BuildEmptyPayload();

/// The discovery file body (§4). Lives here so JSON has exactly one translation unit.
std::string BuildDiscoveryJson(int port, const std::string &name);

}  // namespace bridge
