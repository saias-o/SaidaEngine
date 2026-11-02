#include "saida/p2p/Socket.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using SockLen = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
#endif

#include <cstdio>
#include <mutex>

namespace saida::p2p {
namespace {

#ifdef _WIN32
using Native = SOCKET;
inline Native native(intptr_t h) { return static_cast<Native>(h); }
inline int lastError() { return WSAGetLastError(); }
inline bool wouldBlock(int e) { return e == WSAEWOULDBLOCK; }
// Errors a UDP receive reports for a past datagram, not for the socket: an
// ICMP unreachable (if SIO_UDP_CONNRESET could not be turned off), a datagram
// larger than the buffer, a network reset. The next receive works.
inline bool transient(int e) { return e == WSAECONNRESET || e == WSAEMSGSIZE || e == WSAENETRESET; }
void startup() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#else
using Native = int;
inline Native native(intptr_t h) { return static_cast<Native>(h); }
inline int lastError() { return errno; }
inline bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
inline bool transient(int e) { return e == ECONNREFUSED || e == EINTR || e == EMSGSIZE; }
void startup() {}
#endif

Address fromSockaddr(const sockaddr_storage& sa) {
    if (sa.ss_family == AF_INET) {
        const auto& in = reinterpret_cast<const sockaddr_in&>(sa);
        Address a;
        std::memcpy(a.ip.data(), &in.sin_addr, 4);
        a.port = ntohs(in.sin_port);
        return a;
    }
    if (sa.ss_family == AF_INET6) {
        const auto& in6 = reinterpret_cast<const sockaddr_in6&>(sa);
        return Address::fromIpv6(reinterpret_cast<const uint8_t*>(&in6.sin6_addr), ntohs(in6.sin6_port));
    }
    return {};
}

SockLen toSockaddr(const Address& a, bool v6Socket, sockaddr_storage& sa) {
    std::memset(&sa, 0, sizeof sa);
    if (v6Socket) {
        auto& in6 = reinterpret_cast<sockaddr_in6&>(sa);
        in6.sin6_family = AF_INET6;
        in6.sin6_port = htons(a.port);
        auto* b = reinterpret_cast<uint8_t*>(&in6.sin6_addr);
        if (a.v6) {
            std::memcpy(b, a.ip.data(), 16);
        } else {
            b[10] = b[11] = 0xff;
            std::memcpy(b + 12, a.ip.data(), 4);
        }
        return SockLen(sizeof(sockaddr_in6));
    }
    auto& in = reinterpret_cast<sockaddr_in&>(sa);
    in.sin_family = AF_INET;
    in.sin_port = htons(a.port);
    std::memcpy(&in.sin_addr, a.ip.data(), 4);
    return SockLen(sizeof(sockaddr_in));
}

bool setNonBlocking(Native s) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void closeNative(Native s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

} // namespace

bool UdpSocket::fail(const char* what) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (system error %d)", what, lastError());
    error_ = buf;
    close();
    return false;
}

bool UdpSocket::open(const SocketOptions& o) {
    close();
    startup();
    Native s = static_cast<Native>(kInvalid);
    dualStack_ = false;
    if (o.ipv6) {
        s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (s != static_cast<Native>(kInvalid)) {
            int off = 0;
            if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off) == 0) {
                dualStack_ = true;
            } else {
                closeNative(s);
                s = static_cast<Native>(kInvalid);
            }
        }
    }
    if (s == static_cast<Native>(kInvalid)) s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == static_cast<Native>(kInvalid)) return fail("socket");
    handle_ = static_cast<intptr_t>(s);
    if (!setNonBlocking(s)) return fail("non-blocking mode");
    if (o.reuseAddress) {
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&on), sizeof on);
#endif
    }
    if (o.bufferBytes > 0) {
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&o.bufferBytes), sizeof o.bufferBytes);
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&o.bufferBytes), sizeof o.bufferBytes);
    }
#ifdef _WIN32
    BOOL report = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &bytes, nullptr, nullptr);
#endif
    sockaddr_storage sa;
    Address any;
    any.port = o.port;
    any.v6 = dualStack_;
    const SockLen len = toSockaddr(any, dualStack_, sa);
    if (bind(s, reinterpret_cast<sockaddr*>(&sa), len) != 0) return fail("bind");
    SockLen got = SockLen(sizeof sa);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&sa), &got) != 0) return fail("getsockname");
    local_ = fromSockaddr(sa);
    error_.clear();
    return true;
}

void UdpSocket::close() {
    if (handle_ != kInvalid) closeNative(native(handle_));
    handle_ = kInvalid;
}

bool UdpSocket::send(const Address& to, const uint8_t* data, size_t size) {
    if (handle_ == kInvalid || (to.v6 && !dualStack_)) return ++sendFailures_, false;
    sockaddr_storage sa;
    const SockLen len = toSockaddr(to, dualStack_, sa);
    const auto sent = sendto(native(handle_), reinterpret_cast<const char*>(data), int(size), 0,
                             reinterpret_cast<const sockaddr*>(&sa), len);
    if (sent < 0 || size_t(sent) != size) return ++sendFailures_, false;
    return true;
}

int UdpSocket::receive(Address& from, uint8_t* buf, size_t capacity) {
    if (handle_ == kInvalid) return -1;
    for (int attempts = 0; attempts < 64; ++attempts) {
        sockaddr_storage sa;
        SockLen len = SockLen(sizeof sa);
        const auto got = recvfrom(native(handle_), reinterpret_cast<char*>(buf), int(capacity), 0,
                                  reinterpret_cast<sockaddr*>(&sa), &len);
        if (got >= 0) {
            from = fromSockaddr(sa);
            return int(got);
        }
        const int e = lastError();
        if (wouldBlock(e)) return -1;
        ++receiveErrors_;
        if (!transient(e)) return -1;
    }
    return -1;
}

bool UdpSocket::joinMulticast(const Address& group) {
    if (handle_ == kInvalid || group.v6) return false;
    // An IPv4 group on a dual-stack socket is not portable: LAN discovery
    // opens its own IPv4 socket (ipv6 = false).
    if (dualStack_) return false;
    const Native s = native(handle_);
    ip_mreq m{};
    std::memcpy(&m.imr_multiaddr, group.ip.data(), 4);
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof m) != 0) return false;
#ifdef _WIN32
    const DWORD ttl = 1, loop = 1;  // Windows takes DWORDs, POSIX takes bytes
#else
    const unsigned char ttl = 1, loop = 1;
#endif
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof loop);
    return true;
}

std::vector<Address> resolve(const std::string& host, uint16_t port) {
    startup();
    std::vector<Address> out;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0) return out;
    for (addrinfo* p = list; p; p = p->ai_next) {
        sockaddr_storage sa{};
        std::memcpy(&sa, p->ai_addr, size_t(p->ai_addrlen) < sizeof sa ? size_t(p->ai_addrlen) : sizeof sa);
        Address a = fromSockaddr(sa);
        a.port = port;
        if (!a.isUnspecified()) {
            bool seen = false;
            for (const Address& b : out) seen = seen || b == a;
            if (!seen) out.push_back(a);
        }
    }
    freeaddrinfo(list);
    return out;
}

} // namespace saida::p2p
