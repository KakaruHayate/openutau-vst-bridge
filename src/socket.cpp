#include "socket.h"

#include "log.h"

#include <cerrno>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace bridge {
namespace {

#if defined(_WIN32)

using NativeSocket = SOCKET;

/// Winsock is reference counted per process. The matching WSACleanup is deliberately never
/// called: a plugin can be unloaded while the host still holds sockets of its own, and
/// tearing Winsock down under it would be far worse than leaking one reference.
void EnsureSocketsReady() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            BRIDGE_ERROR("WSAStartup failed with %d; no connection will be possible.", result);
        }
    });
}

int LastError() {
    return WSAGetLastError();
}

bool WouldBlock(int error) {
    return error == WSAEWOULDBLOCK;
}

void CloseHandle(SocketHandle handle) {
    ::closesocket(static_cast<NativeSocket>(handle));
}

#else

using NativeSocket = int;

void EnsureSocketsReady() {}

int LastError() {
    return errno;
}

bool WouldBlock(int error) {
    return error == EWOULDBLOCK || error == EAGAIN || error == EINTR;
}

void CloseHandle(SocketHandle handle) {
    ::close(static_cast<NativeSocket>(handle));
}

#endif

NativeSocket Native(SocketHandle handle) {
    return static_cast<NativeSocket>(handle);
}

/// True on readable, false on timeout or failure. poll() is used on POSIX because a plugin
/// inside a DAW can easily hold descriptors above FD_SETSIZE, which select() cannot express.
bool WaitReadable(SocketHandle handle, int timeoutMs, bool *failed) {
    *failed = false;
#if defined(_WIN32)
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(Native(handle), &readable);
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int result = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
    pollfd entry{};
    entry.fd = Native(handle);
    entry.events = POLLIN;
    int result = ::poll(&entry, 1, timeoutMs);
#endif
    if (result > 0) {
        return true;
    }
    if (result < 0 && !WouldBlock(LastError())) {
        *failed = true;
    }
    return false;
}

void DisableNagle(SocketHandle handle) {
    // The other side sets NoDelay; control lines are small and latency matters more than
    // coalescing them.
    int on = 1;
    ::setsockopt(Native(handle), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char *>(&on), sizeof(on));
}

}  // namespace

SocketStream::~SocketStream() {
    Close();
}

int SocketStream::Read(void *buffer, size_t size, int timeoutMs) {
    if (!IsOpen()) {
        return kStreamFailed;
    }
    bool failed = false;
    if (!WaitReadable(handle_, timeoutMs, &failed)) {
        return failed ? kStreamFailed : 0;
    }
#if defined(_WIN32)
    int read = ::recv(Native(handle_), static_cast<char *>(buffer), static_cast<int>(size), 0);
#else
    ssize_t read = ::recv(Native(handle_), buffer, size, 0);
#endif
    if (read == 0) {
        return kStreamEnded;
    }
    if (read < 0) {
        return WouldBlock(LastError()) ? 0 : kStreamFailed;
    }
    return static_cast<int>(read);
}

bool SocketStream::Write(const void *buffer, size_t size) {
    if (!IsOpen()) {
        return false;
    }
    const char *cursor = static_cast<const char *>(buffer);
    size_t remaining = size;
    while (remaining > 0) {
#if defined(_WIN32)
        int sent = ::send(Native(handle_), cursor, static_cast<int>(remaining), 0);
#else
        // MSG_NOSIGNAL: a SIGPIPE would kill the host, not just the plugin.
        ssize_t sent = ::send(Native(handle_), cursor, remaining, MSG_NOSIGNAL);
#endif
        if (sent > 0) {
            cursor += sent;
            remaining -= static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && WouldBlock(LastError())) {
            continue;
        }
        return false;
    }
    return true;
}

void SocketStream::Shutdown() {
    if (IsOpen()) {
#if defined(_WIN32)
        ::shutdown(Native(handle_), SD_BOTH);
#else
        ::shutdown(Native(handle_), SHUT_RDWR);
#endif
    }
}

void SocketStream::Close() {
    if (IsOpen()) {
        CloseHandle(handle_);
        handle_ = kInvalidSocket;
    }
}

Listener::~Listener() {
    Stop();
}

bool Listener::Start() {
    EnsureSocketsReady();
    SocketHandle handle = static_cast<SocketHandle>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (handle == kInvalidSocket) {
        BRIDGE_ERROR("Could not create a listening socket (error %d).", LastError());
        return false;
    }
    // SO_REUSEADDR is deliberately not set. The port is dynamic so there is nothing to
    // reclaim, and on Windows it would let another process bind the same port and answer in
    // our place.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;  // The OS picks; the discovery file publishes what it picked.
    // Unqualified on purpose, here and below: macOS defines the byte-order conversions as
    // macros, and `::` in front of what one expands to is not a name at all.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(Native(handle), reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != 0) {
        BRIDGE_ERROR("Could not bind a loopback port (error %d).", LastError());
        CloseHandle(handle);
        return false;
    }
    if (::listen(Native(handle), 1) != 0) {
        BRIDGE_ERROR("Could not listen (error %d).", LastError());
        CloseHandle(handle);
        return false;
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int boundSize = sizeof(bound);
#else
    socklen_t boundSize = sizeof(bound);
#endif
    if (::getsockname(Native(handle), reinterpret_cast<sockaddr *>(&bound), &boundSize) != 0) {
        BRIDGE_ERROR("Could not read the bound port (error %d).", LastError());
        CloseHandle(handle);
        return false;
    }
    handle_ = handle;
    port_ = ntohs(bound.sin_port);
    return true;
}

std::unique_ptr<SocketStream> Listener::Accept(int timeoutMs) {
    if (handle_ == kInvalidSocket) {
        return nullptr;
    }
    bool failed = false;
    if (!WaitReadable(handle_, timeoutMs, &failed)) {
        if (failed) {
            BRIDGE_WARN("Waiting for a connection failed (error %d).", LastError());
        }
        return nullptr;
    }
    SocketHandle accepted =
        static_cast<SocketHandle>(::accept(Native(handle_), nullptr, nullptr));
    if (accepted == kInvalidSocket) {
        return nullptr;
    }
    DisableNagle(accepted);
    return std::make_unique<SocketStream>(accepted);
}

void Listener::Stop() {
    if (handle_ != kInvalidSocket) {
        CloseHandle(handle_);
        handle_ = kInvalidSocket;
        port_ = 0;
    }
}

std::unique_ptr<SocketStream> ConnectLoopback(int port) {
    EnsureSocketsReady();
    SocketHandle handle = static_cast<SocketHandle>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (handle == kInvalidSocket) {
        BRIDGE_ERROR("Could not create a client socket (error %d).", LastError());
        return nullptr;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Blocking: the peer is on this machine with the port already listening, so this either
    // completes at once or fails at once.
    if (::connect(Native(handle), reinterpret_cast<const sockaddr *>(&address),
                  sizeof(address)) != 0) {
        BRIDGE_WARN("Could not connect to 127.0.0.1:%d (error %d).", port, LastError());
        CloseHandle(handle);
        return nullptr;
    }
    DisableNagle(handle);
    return std::make_unique<SocketStream>(handle);
}

}  // namespace bridge
