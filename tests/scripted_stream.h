#pragma once

/*
 * A ByteStream whose reads are scripted. Real sockets deliver whatever happened to arrive, so
 * the framing bugs that matter — a line split across two reads, a payload already buffered
 * behind its header, a stall in the middle of either — only reproduce on demand if the test
 * decides where every boundary falls.
 */

#include "stream.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace bridge {
namespace test {

class ScriptedStream final : public ByteStream {
public:
    /// One chunk, delivered by at most one Read (and split further if the caller's buffer is
    /// smaller than the chunk).
    void PushBytes(const std::string &text) {
        Chunk chunk;
        chunk.type = Chunk::Type::Bytes;
        chunk.bytes.assign(text.begin(), text.end());
        chunks_.push_back(std::move(chunk));
    }

    void PushBytes(const std::vector<uint8_t> &bytes) {
        Chunk chunk;
        chunk.type = Chunk::Type::Bytes;
        chunk.bytes = bytes;
        chunks_.push_back(std::move(chunk));
    }

    /// The same bytes, but one Read per byte — the worst case a real socket can produce.
    void PushBytesSingly(const std::string &text) {
        for (char c : text) {
            PushBytes(std::string(1, c));
        }
    }

    /// One Read returns 0, as a socket does when its timeout expires with nothing available.
    void PushTimeout() { chunks_.push_back(Chunk{Chunk::Type::Timeout, {}}); }

    /// A clean close. Sticky: every later Read ends too.
    void PushEnd() { chunks_.push_back(Chunk{Chunk::Type::End, {}}); }

    void PushFailure() { chunks_.push_back(Chunk{Chunk::Type::Failure, {}}); }

    int Read(void *buffer, size_t size, int timeoutMs) override {
        (void)timeoutMs;
        if (ended_) {
            return kStreamEnded;
        }
        if (chunks_.empty()) {
            // Nothing scripted: behave like a quiet socket rather than a closed one, so a
            // reader's own timeout is what ends the wait.
            return 0;
        }
        Chunk &chunk = chunks_.front();
        switch (chunk.type) {
            case Chunk::Type::Timeout:
                chunks_.pop_front();
                return 0;
            case Chunk::Type::End:
                ended_ = true;
                return kStreamEnded;
            case Chunk::Type::Failure:
                chunks_.pop_front();
                return kStreamFailed;
            case Chunk::Type::Bytes:
                break;
        }
        size_t available = std::min(size, chunk.bytes.size() - offset_);
        std::memcpy(buffer, chunk.bytes.data() + offset_, available);
        offset_ += available;
        if (offset_ == chunk.bytes.size()) {
            chunks_.pop_front();
            offset_ = 0;
        }
        return static_cast<int>(available);
    }

    bool Write(const void *buffer, size_t size) override {
        if (writesFail_) {
            return false;
        }
        const char *bytes = static_cast<const char *>(buffer);
        written_.append(bytes, size);
        return true;
    }

    void Shutdown() override { shutdown_ = true; }

    void FailWrites() { writesFail_ = true; }

    /// Everything written so far, framing included.
    const std::string &Written() const { return written_; }

    /// The written control lines, newlines removed. A test that only sends control lines can
    /// read this instead of picking the stream apart.
    std::vector<std::string> WrittenLines() const {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start < written_.size()) {
            size_t breakAt = written_.find('\n', start);
            if (breakAt == std::string::npos) {
                lines.push_back(written_.substr(start));
                break;
            }
            lines.push_back(written_.substr(start, breakAt - start));
            start = breakAt + 1;
        }
        return lines;
    }

    bool WasShutdown() const { return shutdown_; }

private:
    struct Chunk {
        enum class Type { Bytes, Timeout, End, Failure };
        Type type;
        std::vector<uint8_t> bytes;
    };

    std::deque<Chunk> chunks_;
    size_t offset_ = 0;
    std::string written_;
    bool shutdown_ = false;
    bool ended_ = false;
    bool writesFail_ = false;
};

}  // namespace test
}  // namespace bridge
