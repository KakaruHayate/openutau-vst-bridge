#pragma once

/*
 * A listening plugin plus a connected client, for the tests that want the real socket rather
 * than a scripted stream. The client is the test playing OpenUtau's part: it dials, sends, and
 * reads with the same FrameReader the plugin uses, so anything these tests prove holds for
 * both directions of the framing.
 */

#include "connection.h"
#include "messages.h"
#include "reader.h"
#include "socket.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bridge {
namespace test {

/// Long enough that a loaded machine does not fail a test, short enough that a real hang still
/// ends the run.
inline constexpr int kWaitMs = 5000;

/// Records everything the connection decodes, and dictates what it will pull.
class CollectingHandler final : public ConnectionHandler {
public:
    /// What OnPartLayout reports as missing, i.e. what the connection goes on to pull.
    std::vector<std::string> missing;

    std::vector<std::string> ustx;
    std::vector<PartLayout> parts;
    std::vector<std::vector<TrackInfo>> trackUpdates;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> audio;

    void OnUstx(const std::string &document) override { ustx.push_back(document); }

    std::vector<std::string> OnPartLayout(const std::vector<PartLayout> &layout) override {
        parts = layout;
        return missing;
    }

    void OnTracks(const std::vector<TrackInfo> &tracks) override {
        trackUpdates.push_back(tracks);
    }

    void OnAudio(const std::string &hash, std::vector<uint8_t> &&pcm) override {
        audio.emplace_back(hash, std::move(pcm));
    }
};

struct Loopback {
    Listener listener;
    std::unique_ptr<SocketStream> client;
    std::unique_ptr<SocketStream> server;

    /// Binds, dials and accepts. False if any of the three fails, so a test can REQUIRE it.
    bool Open() {
        if (!listener.Start() || listener.Port() == 0) {
            return false;
        }
        client = ConnectLoopback(listener.Port());
        if (client == nullptr) {
            return false;
        }
        server = listener.Accept(kWaitMs);
        return server != nullptr;
    }
};

inline bool WriteLine(SocketStream *stream, const std::string &line) {
    std::string framed = line + "\n";
    return stream->Write(framed.data(), framed.size());
}

/// Whole stereo float32 frames with recognizable contents.
inline std::vector<uint8_t> Pcm(size_t frames, uint8_t seed = 5) {
    std::vector<uint8_t> bytes(frames * sizeof(float) * 2);
    for (size_t i = 0; i < bytes.size(); i++) {
        bytes[i] = static_cast<uint8_t>(i * 13 + seed);
    }
    return bytes;
}

/// Chosen sample values as interleaved little-endian float32, for the tests that check what
/// comes out of the mixer rather than only that the bytes survived (§6.1).
inline std::vector<uint8_t> WirePcm(const std::vector<float> &left,
                                    const std::vector<float> &right) {
    std::vector<uint8_t> bytes(left.size() * 2 * sizeof(float));
    for (size_t i = 0; i < left.size(); i++) {
        float pair[2] = {left[i], right[i]};
        std::memcpy(bytes.data() + i * 2 * sizeof(float), pair, sizeof(pair));
    }
    return bytes;
}

}  // namespace test
}  // namespace bridge
