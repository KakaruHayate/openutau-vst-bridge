#pragma once

/*
 * The TCP side of the transport: a loopback-only listener on an OS-chosen port, and the
 * accepted socket presented as a ByteStream. Platform headers stay in socket.cpp.
 */

#include "stream.h"

#include <cstdint>
#include <memory>

namespace bridge {

#if defined(_WIN32)
using SocketHandle = uintptr_t;
#else
using SocketHandle = int;
#endif

inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(-1);

/// Owns one connected socket.
class SocketStream final : public ByteStream {
public:
    explicit SocketStream(SocketHandle handle) : handle_(handle) {}
    ~SocketStream() override;

    SocketStream(const SocketStream &) = delete;
    SocketStream &operator=(const SocketStream &) = delete;

    int Read(void *buffer, size_t size, int timeoutMs) override;
    bool Write(const void *buffer, size_t size) override;

    /// Half-closes so a blocked peer notices, without releasing the handle.
    void Shutdown() override;
    void Close();
    bool IsOpen() const { return handle_ != kInvalidSocket; }

private:
    SocketHandle handle_;
};

/// Listens on 127.0.0.1 only — the protocol is loopback-trust, so binding anything routable
/// would widen the trust model rather than the feature (PROTOCOL.md §11).
class Listener {
public:
    Listener() = default;
    ~Listener();

    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;

    /// Binds an OS-chosen port and starts listening. False on failure, reason logged.
    bool Start();

    /// The bound port, which is what goes into the discovery file.
    int Port() const { return port_; }

    /// Null on timeout, after Stop(), or on failure. Polls rather than blocking forever so
    /// shutdown does not depend on closing a socket another thread is sitting in.
    std::unique_ptr<SocketStream> Accept(int timeoutMs);

    void Stop();

private:
    SocketHandle handle_ = kInvalidSocket;
    int port_ = 0;
};

/// Dials 127.0.0.1:<port>. In production OpenUtau is the side that connects (PROTOCOL.md §4);
/// this exists so the conformance tests can play OpenUtau over a real socket instead of a
/// simulated stream. Null on failure, reason logged.
std::unique_ptr<SocketStream> ConnectLoopback(int port);

}  // namespace bridge
