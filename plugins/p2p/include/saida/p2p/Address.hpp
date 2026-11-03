// An IPv4 or IPv6 UDP endpoint, and the datagram socket interface every layer
// of this add-on sends through: the operating system's (UdpSocket, Socket.hpp)
// or a simulated Internet with NATs (SimInternet.hpp).
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace saida::p2p {

using Bytes = std::vector<uint8_t>;

struct Address {
    std::array<uint8_t, 16> ip{};  // IPv4 in the first four bytes
    uint16_t port = 0;
    bool v6 = false;

    static Address v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
        Address r;
        r.ip[0] = a, r.ip[1] = b, r.ip[2] = c, r.ip[3] = d;
        r.port = port;
        return r;
    }
    // An IPv4-mapped IPv6 address (::ffff:a.b.c.d) becomes the IPv4 one, so
    // one endpoint has one spelling whatever socket family received it.
    static Address fromIpv6(const uint8_t ip16[16], uint16_t port) {
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        Address r;
        r.port = port;
        if (std::memcmp(ip16, mapped, 12) == 0) {
            std::memcpy(r.ip.data(), ip16 + 12, 4);
        } else {
            std::memcpy(r.ip.data(), ip16, 16);
            r.v6 = true;
        }
        return r;
    }
    bool valid() const { return port != 0 && !(isUnspecified()); }
    bool isUnspecified() const {
        for (size_t i = 0; i < (v6 ? 16u : 4u); ++i)
            if (ip[i]) return false;
        return true;
    }
    bool isLoopback() const {
        if (!v6) return ip[0] == 127;
        for (size_t i = 0; i < 15; ++i)
            if (ip[i]) return false;
        return ip[15] == 1;
    }
    // RFC 1918, link-local, CGNAT (100.64/10) and IPv6 unique-local/link-local.
    bool isPrivate() const {
        if (!v6)
            return ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xf0) == 16) || (ip[0] == 192 && ip[1] == 168) ||
                   (ip[0] == 169 && ip[1] == 254) || (ip[0] == 100 && (ip[1] & 0xc0) == 64);
        return (ip[0] & 0xfe) == 0xfc || (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80);
    }
    bool sameHost(const Address& o) const { return v6 == o.v6 && ip == o.ip; }
    bool operator==(const Address& o) const { return v6 == o.v6 && port == o.port && ip == o.ip; }
    bool operator!=(const Address& o) const { return !(*this == o); }

    std::string toString() const {
        char buf[64];
        if (!v6) {
            std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
        } else {
            int n = std::snprintf(buf, sizeof buf, "[");
            for (int i = 0; i < 16; i += 2)
                n += std::snprintf(buf + n, sizeof buf - size_t(n), "%s%x", i ? ":" : "", unsigned(ip[i] << 8 | ip[i + 1]));
            std::snprintf(buf + n, sizeof buf - size_t(n), "]:%u", port);
        }
        return buf;
    }

    // "a.b.c.d:port" or "[x:x::x]:port" (the full and the :: forms).
    static bool parse(std::string_view s, Address& out) {
        out = Address{};
        auto number = [](std::string_view t, int base, unsigned long max, unsigned long& v) {
            if (t.empty() || t.size() > 5) return false;
            v = 0;
            for (char c : t) {
                int d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 99;
                if (d >= base) return false;
                v = v * unsigned(base) + unsigned(d);
            }
            return v <= max;
        };
        unsigned long port = 0;
        if (!s.empty() && s[0] == '[') {
            const size_t close = s.find(']');
            if (close == std::string_view::npos || close + 1 >= s.size() || s[close + 1] != ':') return false;
            if (!number(s.substr(close + 2), 10, 65535, port)) return false;
            std::string_view h = s.substr(1, close - 1);
            uint16_t groups[8] = {};
            int before = 0, after = 0;
            uint16_t tail[8] = {};
            const size_t gap = h.find("::");
            auto parseGroups = [&](std::string_view part, uint16_t* dst, int& count) {
                count = 0;
                while (!part.empty()) {
                    const size_t colon = part.find(':');
                    unsigned long g;
                    if (count == 8 || !number(part.substr(0, colon), 16, 0xffff, g)) return false;
                    dst[count++] = uint16_t(g);
                    if (colon == std::string_view::npos) break;
                    part.remove_prefix(colon + 1);
                    if (part.empty()) return false;
                }
                return true;
            };
            if (gap == std::string_view::npos) {
                if (!parseGroups(h, groups, before) || before != 8) return false;
            } else {
                if (!parseGroups(h.substr(0, gap), groups, before) || !parseGroups(h.substr(gap + 2), tail, after) ||
                    before + after > 7)
                    return false;
                for (int i = 0; i < after; ++i) groups[8 - after + i] = tail[i];
            }
            uint8_t raw[16];
            for (int i = 0; i < 8; ++i) raw[2 * i] = uint8_t(groups[i] >> 8), raw[2 * i + 1] = uint8_t(groups[i]);
            out = fromIpv6(raw, uint16_t(port));
            return port != 0;
        }
        const size_t colon = s.rfind(':');
        if (colon == std::string_view::npos || !number(s.substr(colon + 1), 10, 65535, port)) return false;
        std::string_view h = s.substr(0, colon);
        for (int i = 0; i < 4; ++i) {
            const size_t dot = h.find('.');
            unsigned long b;
            if ((i < 3) == (dot == std::string_view::npos) || !number(h.substr(0, dot), 10, 255, b)) return false;
            out.ip[size_t(i)] = uint8_t(b);
            h.remove_prefix(dot == std::string_view::npos ? h.size() : dot + 1);
        }
        out.port = uint16_t(port);
        return port != 0;
    }

    // Compact form: 6 bytes for IPv4 (BitTorrent's), 18 for IPv6; network order.
    size_t compactSize() const { return v6 ? 18 : 6; }
    void writeCompact(uint8_t* out) const {
        const size_t n = v6 ? 16 : 4;
        std::memcpy(out, ip.data(), n);
        out[n] = uint8_t(port >> 8), out[n + 1] = uint8_t(port);
    }
    static Address readCompact(const uint8_t* in, size_t size) {
        Address r;
        if (size != 6 && size != 18) return r;
        r.v6 = size == 18;
        const size_t n = r.v6 ? 16 : 4;
        std::memcpy(r.ip.data(), in, n);
        r.port = uint16_t(in[n] << 8 | in[n + 1]);
        return r;
    }

    uint64_t hash() const {
        uint64_t h = 1469598103934665603ULL ^ (v6 ? 6 : 4);
        for (uint8_t b : ip) h = (h ^ b) * 1099511628211ULL;
        return (h ^ port) * 1099511628211ULL;
    }
};

// What every layer sends through. send() never blocks: a datagram the system
// cannot take now is dropped and counted, as the network itself would.
// receive() returns the size of the next waiting datagram (copied into `buf`,
// truncated datagrams are dropped) or -1 when none is waiting.
class DatagramSocket {
public:
    virtual ~DatagramSocket() = default;
    virtual bool send(const Address& to, const uint8_t* data, size_t size) = 0;
    virtual int receive(Address& from, uint8_t* buf, size_t capacity) = 0;
    virtual Address local() const = 0;
    // Blocks until a datagram is waiting or `seconds` have passed (a socket
    // that cannot be waited on just sleeps). For a network thread.
    virtual void wait(double seconds) {
        std::this_thread::sleep_for(std::chrono::duration<double>(std::max(0.0, seconds)));
    }
};

} // namespace saida::p2p
