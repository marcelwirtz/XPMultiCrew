#pragma once

// Minimal cross-platform UDP wrapper for the Phase 0 spike. Deliberately
// tiny (blocking send/receive, no framing beyond what callers do themselves)
// - the real transport (QUIC/ENet, reliable+unreliable channels, see
// docs/plan.md section 7) replaces this in a later phase.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <optional>
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

// Outcome of SendToAsync - see there.
enum class SendStatus {
    kSent,      // handed to the OS
    kResolving, // host name still being looked up in the background, nothing sent
    kFailed,    // lookup failed (retried after kResolveRetryDelay) or sendto() failed
};

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

    // Never blocks on DNS: a dotted IPv4 address is used directly, a host
    // name is looked up on a background thread and kResolving returned
    // (nothing sent) until that finishes. Callers that must not lose the
    // datagram (RendezvousClient) queue it and retry; everything sent at
    // frame rate goes to numeric peer addresses anyway. getaddrinfo used to
    // run right here on X-Plane's main thread, stalling the sim for as long
    // as a slow or broken DNS server took to answer.
    SendStatus SendToAsync(const std::string& host, uint16_t port, const void* data, size_t len) {
        sockaddr_in addr{};
        const SendStatus resolved = ResolveHostPort(host, port, addr);
        if (resolved != SendStatus::kSent) {
            return resolved;
        }
        const auto sent = sendto(socket_, reinterpret_cast<const char*>(data),
                                  static_cast<int>(len), 0,
                                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        return sent >= 0 && static_cast<size_t>(sent) == len ? SendStatus::kSent : SendStatus::kFailed;
    }

    bool SendTo(const std::string& host, uint16_t port, const void* data, size_t len) {
        return SendToAsync(host, port, data, len) == SendStatus::kSent;
    }

    // How long a failed lookup is remembered before the next attempt.
    static constexpr std::chrono::seconds kResolveRetryDelay{30};

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
    // to a sockaddr_in, caching the result per "host:port". Returns kSent
    // once `out` is filled. Doesn't re-resolve on a cache hit even if the
    // underlying DNS record changed - fine for a VPS with a stable IP.
    SendStatus ResolveHostPort(const std::string& host, uint16_t port, sockaddr_in& out) {
        const std::string key = host + ":" + std::to_string(port);
        const auto cached = resolve_cache_.find(key);
        if (cached != resolve_cache_.end()) {
            out = cached->second;
            return SendStatus::kSent;
        }

        sockaddr_in numeric{};
        numeric.sin_family = AF_INET;
        numeric.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &numeric.sin_addr) == 1) {
            resolve_cache_.emplace(key, numeric);
            out = numeric;
            return SendStatus::kSent;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto failed = failed_until_.find(key);
        if (failed != failed_until_.end()) {
            if (now < failed->second) {
                return SendStatus::kFailed;
            }
            failed_until_.erase(failed);
        }

        auto pending = pending_.find(key);
        if (pending == pending_.end()) {
            pending_.emplace(key, std::async(std::launch::async, &UdpSocket::BlockingResolve, host, port));
            return SendStatus::kResolving;
        }
        if (pending->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return SendStatus::kResolving;
        }
        const std::optional<sockaddr_in> result = pending->second.get();
        pending_.erase(pending);
        if (!result) {
            failed_until_[key] = now + kResolveRetryDelay;
            return SendStatus::kFailed;
        }
        resolve_cache_.emplace(key, *result);
        out = *result;
        return SendStatus::kSent;
    }

    // Runs on its own thread (see ResolveHostPort), so it takes its own
    // Winsock reference instead of relying on this socket staying open.
    static std::optional<sockaddr_in> BlockingResolve(std::string host, uint16_t port) {
#if defined(_WIN32)
        WSADATA wsaData;
        const bool wsa = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#endif
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        std::optional<sockaddr_in> resolved;
        addrinfo* result = nullptr;
        const std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) == 0 && result) {
            sockaddr_in addr{};
            std::memcpy(&addr, result->ai_addr, sizeof(addr));
            resolved = addr;
        }
        if (result) {
            freeaddrinfo(result);
        }
#if defined(_WIN32)
        if (wsa) WSACleanup();
#endif
        return resolved;
    }

    socket_t socket_ = kInvalidSocket;
    std::unordered_map<std::string, sockaddr_in> resolve_cache_;
    // In-flight lookups. A std::async future's destructor waits for its
    // thread, so destroying the socket (plugin unload) never leaves a
    // thread running in code that's about to be unmapped.
    std::unordered_map<std::string, std::future<std::optional<sockaddr_in>>> pending_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> failed_until_;
#if defined(_WIN32)
    bool wsa_initialized_ = false;
#endif
};

} // namespace flytogether
