/*
 * The bridge outside a DAW: a Session, a fake transport and a fake audio thread, so OpenUtau's
 * real client can be run against the real plugin code without a host in between.
 *
 * Both sides of this protocol are otherwise only ever tested against a stand-in — the plugin's
 * tests speak to a FakeOpenUtau, OpenUtau's speak to a DawTestPlugin — and two implementations
 * that each agree with their own fake can still disagree with each other. This is what closes
 * that: start it, connect OpenUtau's DAW dialog to it, and watch the peak meter come alive as
 * parts arrive.
 *
 * Not part of the shipped plugin. Build it with -DBRIDGE_BUILD_HOST=ON.
 */

#include "clock.h"
#include "session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void OnInterrupt(int) {
    g_stop.store(true);  // Async-signal-safe: a lock-free store and nothing else.
}

struct Options {
    double sampleRate = 48000.0;  // Not the wire rate, so a real run exercises the resampler.
    int trackNo = 0;
    uint32_t blockFrames = 512;
    double loopSeconds = 0.0;  // 0 plays forward forever.
    std::string directory;     // Empty means the production discovery directory.
};

/// Returns false when the arguments do not make sense, having said why.
bool ParseOptions(int argc, char **argv, Options *options) {
    for (int i = 1; i < argc; i++) {
        std::string flag = argv[i];
        bool hasValue = i + 1 < argc;
        if (flag == "--help" || flag == "-h") {
            return false;
        }
        if (flag == "--rate" && hasValue) {
            options->sampleRate = std::atof(argv[++i]);
        } else if (flag == "--track" && hasValue) {
            options->trackNo = std::atoi(argv[++i]);
        } else if (flag == "--block" && hasValue) {
            options->blockFrames = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (flag == "--loop" && hasValue) {
            options->loopSeconds = std::atof(argv[++i]);
        } else if (flag == "--dir" && hasValue) {
            options->directory = argv[++i];
        } else {
            std::printf("Unrecognised argument: %s\n", flag.c_str());
            return false;
        }
    }
    if (options->sampleRate < 8000.0 || options->blockFrames == 0) {
        std::printf("A sample rate of at least 8000 and a non-zero block size are needed.\n");
        return false;
    }
    return true;
}

void Usage() {
    std::printf(
        "Runs the bridge's session outside a DAW so OpenUtau can connect to it.\n\n"
        "  --rate <hz>      host sample rate to convert to (default 48000)\n"
        "  --track <n>      the OpenUtau track to play, zero-based (default 0)\n"
        "  --block <n>      frames per block (default 512)\n"
        "  --loop <sec>     restart the transport every <sec> seconds (default: play forward)\n"
        "  --dir <path>     discovery directory (default: the one OpenUtau scans)\n");
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        Usage();
        return 1;
    }

    std::signal(SIGINT, OnInterrupt);
    std::signal(SIGTERM, OnInterrupt);

    // Constructed with the directory rather than reassigned into: a Session owns a thread and a
    // listening socket, so it is neither copyable nor movable.
    std::filesystem::path directory = options.directory.empty()
                                          ? bridge::DiscoveryDirectory()
                                          : std::filesystem::path(options.directory);
    bridge::Session session(std::move(directory));
    session.SetHostSampleRate(options.sampleRate);
    session.SetTrackNo(options.trackNo);
    if (!session.Start()) {
        std::printf("Could not listen or advertise; nothing to connect to.\n");
        return 1;
    }
    std::printf("Listening on port %d at %.0f Hz, playing track %d. Ctrl-C to stop.\n",
                session.Port(), options.sampleRate, options.trackNo + 1);

    std::vector<float> left(options.blockFrames, 0.0f);
    std::vector<float> right(options.blockFrames, 0.0f);
    // A transport that is playing from the first block, which is also the edge that makes the
    // session send playbackStarted. The host's own tempo is reported once it is known.
    session.NoteTransport(true, 0.0, true, false, 0.0);

    const int64_t startMs = bridge::NowMs();
    // Monotonic, for pacing; the position it renders at is this folded into the loop, so a part
    // placed anywhere in the loop comes round again however late something connects.
    const int64_t loopFrames =
        static_cast<int64_t>(std::llround(options.loopSeconds * options.sampleRate));
    int64_t rendered = 0;
    int64_t reportedSecond = -1;
    int64_t lastPosition = 0;
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    while (!g_stop.load()) {
        int64_t position = loopFrames > 0 ? rendered % loopFrames : rendered;
        if (position < lastPosition) {
            // The transport jumped back to the top of the loop, which a DAW reports as a fresh
            // start — and a start is what makes the session send playbackStarted, so looping is
            // also how that notification gets exercised.
            session.NoteTransport(false, 0.0, false, false, 0.0);
            session.NoteTransport(true, 0.0, true, false, 0.0);
        }
        // The position feeds the v1.1 playhead notifications as well as the renderer.
        session.NoteTransport(true, static_cast<double>(position) / options.sampleRate, true,
                              false, 0.0);
        lastPosition = position;
        session.Render(position, left.size(), left.data(), right.data());
        for (size_t i = 0; i < left.size(); i++) {
            peakLeft = std::max(peakLeft, std::fabs(left[i]));
            peakRight = std::max(peakRight, std::fabs(right[i]));
        }
        rendered += static_cast<int64_t>(left.size());

        int64_t second = position / static_cast<int64_t>(options.sampleRate);
        if (second != reportedSecond) {
            // Labelled with the second that just finished, which is the one the peak covers.
            if (reportedSecond >= 0) {
                std::printf("t=%3llds  peak L %.4f  R %.4f%s\n",
                            static_cast<long long>(reportedSecond), peakLeft, peakRight,
                            session.IsConnected() ? "  [OpenUtau connected]" : "");
                std::fflush(stdout);
            }
            reportedSecond = second;
            peakLeft = 0.0f;
            peakRight = 0.0f;
        }

        // Paced against the wall clock, because a session that renders as fast as it can would
        // reach the end of the project before OpenUtau had finished sending it.
        int64_t dueMs = startMs + rendered * 1000 / static_cast<int64_t>(options.sampleRate);
        int64_t waitMs = dueMs - bridge::NowMs();
        if (waitMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        }
    }

    std::printf("Stopping.\n");
    session.NoteTransport(false, 0.0, false, false, 0.0);
    session.Stop();
    return 0;
}
