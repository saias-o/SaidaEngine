// Encrypted peer links over one UDP socket: the transport a rollback session
// (plugins/netcode) or any other game protocol runs on, with no server.
//
//   handshake   Noise XX: each side proves its static key; the initiator's
//               first packet is padded so no reply is larger than the request
//               (no amplification), and a responder spends no key agreement
//               on a request its admission policy or rate budget refuses.
//   packets     ChaCha20-Poly1305 with the packet number as nonce; a 256-
//               packet window refuses replays and duplicates. 25 bytes of
//               overhead on top of UDP.
//   channels    Unreliable (delivered at once, or never) and Reliable
//               (delivered once, in order, retransmitted until acknowledged),
//               coalesced into as few datagrams as fit the MTU at flush();
//               messages larger than a datagram are fragmented by the link,
//               never by IP.
//   NAT         connect() repeats its first packet, so two peers connecting
//               to each other at the same time open both NATs (hole punching);
//               the simultaneous opens resolve to one session. A connected
//               peer can introduce two of its peers to each other, so they
//               punch at the same instant. Each side tells the other the
//               address it sees (the public address behind a NAT).
//   liveness    acknowledgements give the round trip, its variance and the
//               loss rate; keepalives hold NAT mappings open; a silent peer
//               times out; a peer that restarted is a new peer.
//
// Single-threaded and poll-driven: poll(now) receives and runs timers,
// send() queues, flush() sends. Nothing blocks.
#pragma once

#include "saida/p2p/Address.hpp"
#include "saida/p2p/Crypto.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace saida::p2p {

// A connected peer, local to one Host. It doubles as netcode's PeerId.
using PeerHandle = uint32_t;
inline constexpr PeerHandle kNoPeer = 0;

enum class Channel : uint8_t { Unreliable, Reliable };

// A stable identity: an X25519 key pair. id() is the participant id the game
// uses (netcode's PlayerId): the first eight bytes of a hash of the public key.
struct Identity {
    KeyPair keys;
    static Identity generate() { return {KeyPair::generate()}; }
    static uint64_t idOf(const Key& pub);
    uint64_t id() const { return idOf(keys.pub); }
};

struct HostConfig {
    std::string application = "saida";  // hashed into every handshake: apps never cross
    Bytes hello;                        // shown to every peer once authenticated (<= 128 bytes)
    size_t mtu = 1200;                  // largest datagram sent (QUIC's safe minimum)
    size_t maxPeers = 64;
    size_t maxHandshakes = 64;          // in progress at once
    double handshakeRate = 64;          // key agreements answered per second (burst: as many)
    double connectSeconds = 10;         // a connect() gives up after this
    double retrySeconds = 0.1;          // first handshake retry; doubles up to 1 s
    double keepaliveSeconds = 1.0;      // an idle link still sends this often
    double timeoutSeconds = 10;         // a peer silent this long is gone
    double ackDelaySeconds = 0.005;     // an acknowledgement waits at most this for a ride
    size_t maxMessage = 1 << 20;        // largest reliable message
    size_t maxUnreliable = 16 * 1024;   // largest unreliable message (fragmented, all or nothing)
    size_t maxQueuedBytes = 4 << 20;    // per peer, reliable bytes not yet acknowledged
    size_t maxInflightBytes = 256 << 10;// per peer, bytes sent and not yet acknowledged
    size_t maxPacketsPerFlush = 64;     // per peer
    size_t receiveBudget = 4096;        // datagrams read per poll
    double introductionsPerSecond = 4;  // accepted from one peer
};

enum class Reason : uint8_t {
    None,
    // Why a peer went away, or a connect() failed.
    Closed, Timeout, Restarted, ConnectTimeout, WrongKey, Refused,
    // Why a datagram or a send was refused.
    Malformed, BadVersion, UnknownIndex, AuthFailed, Replayed, RateLimited, TooManyPeers,
    TooManyHandshakes, NotAdmitted, HandshakeFailed, TooLarge, QueueFull, UnknownPeer, Foreign,
    Count
};
const char* reasonName(Reason r);

enum class HostEventType : uint8_t {
    Connected,      // a new authenticated peer
    Disconnected,   // reason: Closed (either side), Timeout, Restarted
    ConnectFailed,  // a connect() to `address` ended: ConnectTimeout, WrongKey, Refused
    Introduced,     // a peer introduced `address` to us; a connect() is under way
};

struct HostEvent {
    HostEventType type;
    PeerHandle peer = kNoPeer;
    Address address;
    Reason reason = Reason::None;
    Bytes intent;  // Connected: what the initiator asked for
};

struct PeerInfo {
    PeerHandle handle = kNoPeer;
    Address address;            // where it is reached now (follows NAT rebinding)
    Key key{};                  // its static public key
    uint64_t id = 0;            // Identity::idOf(key)
    Bytes hello;                // its HostConfig::hello
    Bytes intent;               // the initiator's intent for this link
    bool initiator = false;     // we connected to it
    double rtt = 0, rttVariance = 0, loss = 0;
    double connectedAt = 0, lastReceive = 0;
    uint64_t packetsSent = 0, packetsReceived = 0, packetsLost = 0, retransmits = 0;
    uint64_t bytesSent = 0, bytesReceived = 0;
    size_t queuedBytes = 0, inflightBytes = 0;
};

struct HostCounters {
    uint64_t datagramsSent = 0, datagramsReceived = 0, bytesSent = 0, bytesReceived = 0;
    uint64_t handshakesStarted = 0, handshakesAnswered = 0, handshakesCompleted = 0;
    uint64_t refused[size_t(Reason::Count)] = {};
};

class Host {
public:
    using Receiver = std::function<void(PeerHandle, const uint8_t*, size_t, Channel)>;
    // Before any key agreement: may a handshake from `from` asking `intent` proceed?
    using Admission = std::function<bool(const Address& from, const Bytes& intent)>;
    // A datagram that is not this protocol's (a DHT message on the same port).
    using Foreign = std::function<void(const Address& from, const uint8_t*, size_t)>;

    Host(DatagramSocket& socket, const Identity& identity, const HostConfig& config = {});
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Starts (or keeps) a handshake with `to`. `intent` (<= 64 bytes) is seen
    // by the other side before it agrees; `expect` refuses any other key.
    // False when refused at once (limits, self); otherwise a Connected or a
    // ConnectFailed event follows. Connecting to a connected address is a no-op.
    bool connect(const Address& to, const Bytes& intent = {}, const Key* expect = nullptr);
    void disconnect(PeerHandle peer);

    // Queues a message. False (and counted) when the peer is unknown, the
    // message too large or the peer's queue full.
    bool send(PeerHandle peer, const uint8_t* data, size_t size, Channel channel);
    bool send(PeerHandle peer, const Bytes& data, Channel channel) { return send(peer, data.data(), data.size(), channel); }
    // Tells `a` and `b` about each other: both connect at once, which opens
    // most NAT pairs that cannot reach each other first.
    void introduce(PeerHandle a, PeerHandle b, const Bytes& intent);

    void poll(double now);
    void flush();

    void setReceiver(Receiver r);
    // Every event is also handed to each listener, at the end of poll(), in
    // order (layers such as Rendezvous listen; the game still takes them).
    void addListener(std::function<void(const HostEvent&)> listener);
    // Reliable messages for a layer built on the host (Rendezvous is service
    // 1; 255 is the host's own), ordered with the game's reliable messages
    // but never shown to it.
    // At most singleDatagramMessage() - 1 bytes.
    using Service = std::function<void(PeerHandle, const uint8_t*, size_t)>;
    void setService(uint8_t service, Service handler);
    bool sendService(PeerHandle peer, uint8_t service, const uint8_t* data, size_t size);
    void setAdmission(Admission a);
    void setForeign(Foreign f);
    // Sends a datagram outside any link (a DHT query), from this socket.
    bool sendRaw(const Address& to, const uint8_t* data, size_t size);

    std::vector<HostEvent> takeEvents();
    const PeerInfo* peer(PeerHandle handle) const;
    PeerHandle peerAt(const Address& address) const;
    PeerHandle peerWithKey(const Key& key) const;
    std::vector<PeerHandle> peers() const;
    bool connecting(const Address& to) const;
    // The address our peers see most often for us; invalid until one says.
    Address publicAddress() const;
    void notePublicAddress(const Address& seen);  // other sources (the DHT's "ip")
    Address localAddress() const;

    const Identity& identity() const;
    const HostConfig& config() const;
    const HostCounters& counters() const;
    // The largest message that always travels in one datagram.
    size_t singleDatagramMessage() const;
    double now() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saida::p2p
