#pragma once

/*
 * The byte channel the framing sits on. Abstract so the reader can be driven from memory in
 * tests: a data frame arriving split across two reads, or a header buffered ahead of its
 * payload, is where framing bugs live, and only a scripted stream reproduces those on demand.
 */

#include <cstddef>

namespace bridge {

class ByteStream {
public:
    virtual ~ByteStream() = default;

    /// Blocks up to `timeoutMs` for at least one byte. Returns the count read, 0 if the
    /// timeout expired with nothing available, -1 at a clean end of stream, and -2 on error.
    virtual int Read(void *buffer, size_t size, int timeoutMs) = 0;

    /// Writes all of `size` or fails. Callers must serialize their own writes.
    virtual bool Write(const void *buffer, size_t size) = 0;

    /// Half-closes both directions so a peer blocked on a read notices the end instead of
    /// waiting out its own timeout. A no-op by default: a memory-backed stream has no peer.
    virtual void Shutdown() {}
};

inline constexpr int kStreamEnded = -1;
inline constexpr int kStreamFailed = -2;

}  // namespace bridge
