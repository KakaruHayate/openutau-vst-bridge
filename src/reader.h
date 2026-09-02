#pragma once

/*
 * Reads the two planes off one stream: LF-terminated UTF-8 control lines and exact-length
 * binary payloads, sharing a single buffer so a data frame that is partially buffered behind
 * its header is not lost (PROTOCOL.md §5). A direct counterpart of `DawFrameReader`.
 */

#include "stream.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bridge {

enum class ReadStatus {
    Ok,
    /// Nothing complete yet. The caller keeps its periodic work going and comes back.
    Timeout,
    /// The peer closed cleanly between messages.
    EndOfStream,
    /// Framing violation. Unrecoverable — the stream position is lost (§8).
    ProtocolError,
};

class FrameReader {
public:
    /// USTX control lines are legitimately multi-MB, so the guard is generous; it exists only
    /// to stop a peer that never sends a newline from exhausting memory.
    static constexpr size_t kMaxLineBytes = 256u * 1024u * 1024u;

    /// A payload is only ever read after we asked for it, so a peer that stalls mid-frame is
    /// a dead connection rather than a slow one.
    static constexpr int kPayloadTimeoutMs = 10000;

    explicit FrameReader(ByteStream *stream) : stream_(stream) {}

    ReadStatus ReadLine(std::string *line, int timeoutMs);

    /// Reads exactly `count` bytes of a data-plane payload.
    ReadStatus ReadExactly(size_t count, std::vector<uint8_t> *payload);

    /// The last framing violation, for the disconnect log.
    const std::string &Error() const { return error_; }

private:
    /// Ok when at least one byte was appended to the buffer.
    ReadStatus Fill(int timeoutMs);

    ByteStream *stream_;
    std::vector<uint8_t> buffer_ = std::vector<uint8_t>(16u * 1024u);
    size_t start_ = 0;
    size_t end_ = 0;
    std::string error_;
};

}  // namespace bridge
