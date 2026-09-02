#include "messages.h"

#include "hash.h"

#include <nlohmann/json.hpp>

namespace bridge {
namespace {

using Json = nlohmann::json;

/// Non-throwing parse. A malformed payload is a warning on the other side too, never fatal.
Json ParseObject(const std::string &text) {
    Json value = Json::parse(text, nullptr, false);
    return value.is_object() ? value : Json();
}

/// Reads a field only if it is actually the requested type, so a peer that sends
/// `"startMs": null` yields the default rather than throwing.
double ReadNumber(const Json &object, const char *key, double fallback) {
    auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<double>() : fallback;
}

int ReadInt(const Json &object, const char *key, int fallback) {
    auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

bool ReadBool(const Json &object, const char *key, bool fallback) {
    auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::string ReadString(const Json &object, const char *key) {
    auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

bool ParseUstxPayload(const std::string &json, std::string *ustx) {
    Json object = ParseObject(json);
    if (object.is_null()) {
        return false;
    }
    auto it = object.find("ustx");
    if (it == object.end() || !it->is_string()) {
        return false;
    }
    *ustx = it->get<std::string>();
    return true;
}

bool ParsePartLayoutRequest(const std::string &json, std::vector<PartLayout> *parts) {
    Json object = ParseObject(json);
    if (object.is_null()) {
        return false;
    }
    auto it = object.find("parts");
    if (it == object.end() || !it->is_array()) {
        return false;
    }
    parts->clear();
    parts->reserve(it->size());
    for (const Json &entry : *it) {
        if (!entry.is_object()) {
            return false;
        }
        PartLayout part;
        part.trackNo = ReadInt(entry, "trackNo", 0);
        part.startMs = ReadNumber(entry, "startMs", 0.0);
        part.endMs = ReadNumber(entry, "endMs", 0.0);
        part.audioHash = ReadString(entry, "audioHash");
        // An unparseable hash cannot be deduped or pulled, so it is a shape error rather
        // than something to carry forward as an empty string.
        if (!TryParseHash(part.audioHash, nullptr)) {
            return false;
        }
        parts->push_back(std::move(part));
    }
    return true;
}

bool ParseTracksNotification(const std::string &json, std::vector<TrackInfo> *tracks) {
    Json object = ParseObject(json);
    if (object.is_null()) {
        return false;
    }
    auto it = object.find("tracks");
    if (it == object.end() || !it->is_array()) {
        return false;
    }
    tracks->clear();
    tracks->reserve(it->size());
    for (const Json &entry : *it) {
        if (!entry.is_object()) {
            return false;
        }
        TrackInfo track;
        track.name = ReadString(entry, "name");
        track.volume = ReadNumber(entry, "volume", 0.0);
        track.pan = ReadNumber(entry, "pan", 0.0);
        // Absent means audible: a minor-older peer that predates the field still mixes.
        track.muted = ReadBool(entry, "muted", false);
        tracks->push_back(std::move(track));
    }
    return true;
}

bool ParseEnvelope(const std::string &json, Envelope *envelope) {
    Json object = ParseObject(json);
    if (object.is_null()) {
        return false;
    }
    auto success = object.find("success");
    if (success == object.end() || !success->is_boolean()) {
        return false;
    }
    envelope->success = success->get<bool>();
    envelope->error = ReadString(object, "error");
    return true;
}

std::string BuildInitResponseEnvelope() {
    Json envelope = {
        {"success", true},
        {"data", {{"apiVersion", kApiVersion}}},
        {"error", nullptr},
    };
    return envelope.dump();
}

std::string BuildPartLayoutResponseEnvelope(const std::vector<std::string> &missingAudios) {
    Json envelope = {
        {"success", true},
        {"data", {{"missingAudios", missingAudios}}},
        {"error", nullptr},
    };
    return envelope.dump();
}

std::string BuildFailEnvelope(const std::string &error) {
    Json envelope = {
        {"success", false},
        {"data", nullptr},
        {"error", error},
    };
    return envelope.dump();
}

std::string BuildGetAudioPayload(const std::string &hash) {
    return Json{{"hash", hash}}.dump();
}

std::string BuildEmptyPayload() {
    return "{}";
}

std::string BuildDiscoveryJson(int port, const std::string &name) {
    Json info = {
        {"port", port},
        {"name", name},
        {"apiVersion", kApiVersion},
    };
    return info.dump();
}

}  // namespace bridge
