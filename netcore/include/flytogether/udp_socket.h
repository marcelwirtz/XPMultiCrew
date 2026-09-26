#pragma once

// Minimal cross-platform UDP wrapper for the Phase 0 spike. Deliberately
// tiny (blocking send/receive, no framing beyond what callers do themselves)
// - the real transport (QUIC/ENet, reliable+unreliable channels, see
// docs/plan.md section 7) replaces this in a later phase.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#include <ws2tcpip.h>
#include <winsock2.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace flytogether {

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { Close(); }

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool Open() {
#if defined(_WIN32)
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return false;
        }
        wsa_initialized_ = true;
#endif
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        return socket_ != kInvalidSocket;
    }

    // `loopbackOnly` binds to 127.0.0.1 instead of every interface - for
    // sockets that must only ever be reachable from this machine (the
    // companion-app control channel, see control/control_listener.h).
    bool Bind(uint16_t port, bool loopbackOnly = false) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        addr.sin_port = htons(port);
        return bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool SetNonBlocking(bool nonBlocking) {
#if defined(_WIN32)
        u_long mode = nonBlocking ? 1 : 0;
        return ioctlsocket(socket_, FIONBIO, &mode) == 0;
#else
        int flags = fcntl(socket_, F_GETFL, 0);
        if (flags == -1) return false;
        flags = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        return fcntl(socket_, F_SETFL, flags) == 0;
#endif
    }

    bool SendTo(const std::string& host, uint16_t port, const void* data, size_t len) {
        sockaddr_in addr{};
        if (!ResolveHostPort(host, port, addr)) {
            return false;
        }
        const auto sent = sendto(socket_, reinterpret_cast<const char*>(data),
                                  static_cast<int>(len), 0,
                                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        return sent >= 0 && static_cast<size_t>(sent) == len;
    }

    // Returns bytes received, 0 on orderly close, or a negative value if
    // nothing was available (non-blocking) or on error.
    // outHost/outPort, if given, receive the sender's address - mainly
    // useful for tests that need to reply to whatever ephemeral local
    // port a client's own socket happened to bind (see
    // tests/rendezvous_client_test.cpp), since callers otherwise don't
    // get to see the peer address a datagram arrived from.
    int ReceiveFrom(void* buffer, size_t buflen, std::string* outHost = nullptr,
                    uint16_t* outPort = nullptr) {
        sockaddr_in from{};
#if defined(_WIN32)
        int fromlen = sizeof(from);
#else
        socklen_t fromlen = sizeof(from);
#endif
        const auto received = recvfrom(socket_, reinterpret_cast<char*>(buffer),
                                        static_cast<int>(buflen), 0,
                                        reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (received >= 0) {
            if (outHost) {
                char host_buf[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from.sin_addr, host_buf, sizeof(host_buf));
                *outHost = host_buf;
            }
            if (outPort) {
                *outPort = ntohs(from.sin_port);
            }
        }
        return static_cast<int>(received);
    }

    void Close() {
        if (socket_ != kInvalidSocket) {
#if defined(_WIN32)
            closesocket(socket_);
#else
            close(socket_);
#endif
            socket_ = kInvalidSocket;
        }
#if defined(_WIN32)
        if (wsa_initialized_) {
            WSACleanup();
            wsa_initialized_ = false;
        }
#endif
    }

private:
    // Resolves "host" (a dotted IPv4 address or a DNS name - e.g. a VPS's
    // hostname for the rendezvous server, see formation/rendezvous_client.h)
    // to a sockaddr_in, caching the result per "host:port" so repeated
    // sends (this can be called at 20 Hz, see plugin_main.cpp) don't each
    // trigger a fresh DNS lookup. Doesn't re-resolve on a cache hit even if
    // the underlying DNS record changed - fine for a VPS with a stable IP,
    // not for a host whose address changes while the plugin is running.
    bool ResolveHostPort(const std::string& host, uint16_t port, sockaddr_in& out) {
        const std::string key = host + ":" + std::to_string(port);
        const auto cached = resolve_cache_.find(key);
        if (cached != resolve_cache_.end()) {
            out = cached->second;
            return true;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo* result = nullptr;
        const std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0 || !result) {
            return false;
        }

        sockaddr_in resolved{};
        std::memcpy(&resolved, result->ai_addr, sizeof(resolved));
        freeaddrinfo(result);

        resolve_cache_.emplace(key, resolved);
        out = resolved;
        return true;
    }

    socket_t socket_ = kInvalidSocket;
    std::unordered_map<std::string, sockaddr_in> resolve_cache_;
#if defined(_WIN32)
    bool wsa_initialized_ = false;
#endif
};

} // namespace flytogether
