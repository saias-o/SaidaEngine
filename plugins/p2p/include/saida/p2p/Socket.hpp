// The operating system's UDP socket: non-blocking, dual-stack (one socket
// reaches IPv4 and IPv6 peers), with the platform traps handled -- Windows
// reports an ICMP "port unreachable" as a receive error that would otherwise
// stall the socket (SIO_UDP_CONNRESET is turned off).
//
// Native only: the Web runtime has no raw UDP (SPEC 11.7), and nothing here
// pretends otherwise.
#pragma once

#include "saida/p2p/Address.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace saida::p2p {

struct SocketOptions {
    uint16_t port = 0;            // 0: any free port
    bool ipv6 = true;             // dual-stack when the system allows it
    bool reuseAddress = false;    // several sockets on one port (LAN multicast)
    int bufferBytes = 1 << 20;    // kernel send and receive buffers
};

class UdpSocket final : public DatagramSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() override { close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // False with error() naming the system call and code.
    bool open(const SocketOptions& options = {});
    void close();
    bool isOpen() const { return handle_ != kInvalid; }

    bool send(const Address& to, const uint8_t* data, size_t size) override;
    int receive(Address& from, uint8_t* buf, size_t capacity) override;
    Address local() const override { return local_; }
    void wait(double seconds) override;  // select(): woken by the first datagram

    // IPv4 multicast for LAN discovery: membership on every interface, loop
    // back to this machine, never routed past the link (TTL 1).
    bool joinMulticast(const Address& group);
    bool dualStack() const { return dualStack_; }

    const std::string& error() const { return error_; }
    uint64_t sendFailures() const { return sendFailures_; }
    uint64_t receiveErrors() const { return receiveErrors_; }

private:
    static constexpr intptr_t kInvalid = -1;
    bool fail(const char* what);

    intptr_t handle_ = kInvalid;
    bool dualStack_ = false;
    Address local_;
    std::string error_;
    uint64_t sendFailures_ = 0, receiveErrors_ = 0;
};

// Name resolution (getaddrinfo). It blocks for as long as the system resolver
// does: call it off the frame loop (Rendezvous does, on its own thread).
std::vector<Address> resolve(const std::string& host, uint16_t port);

} // namespace saida::p2p
