// A Kademlia node of BitTorrent's Mainline DHT (BEP 5): millions of nodes run
// by nobody in particular, reachable from anywhere, which is what lets two
// players find each other with no server of their own. Supported: ping,
// find_node, get_peers, announce_peer with implied_port (the NAT's public
// port is what gets stored), BEP 42's "ip" (the address the network sees for
// us) and BEP 43's read-only flag (a node behind a NAT asks without being
// asked). IPv4 only (BEP 32 is not implemented).
//
// It shares the game's UDP socket (Host::setForeign / sendRaw): the port the
// DHT stores for us is then the very port our peers must punch towards.
//
// watch(key) is the one operation games need: look the key up, announce us
// under it, then keep asking the closest nodes so that every peer that
// announces itself later is found within `refreshSeconds`.
#pragma once

#include "saida/p2p/Address.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace saida::p2p {

using NodeId = std::array<uint8_t, 20>;

struct DhtConfig {
    bool readOnly = true;          // BEP 43: right behind a NAT; a public node serves
    size_t k = 8;                  // bucket size and lookup width
    int alpha = 4;                 // queries in flight per lookup
    double queryTimeout = 2.0;
    double slowSeconds = 0.6;      // a lookup stops waiting for a node this slow
    double queriesPerSecond = 64;  // outgoing budget
    double announceSeconds = 300;  // re-announce under a watched key
    double relookupSeconds = 120;  // a watch walks the network again this often
    size_t maxNodes = 2048;        // routing table
    size_t maxStoredKeys = 4096;   // announcements kept for others (as a node)
    size_t maxPeersPerKey = 128;
    double peerSeconds = 1800;     // an announcement is forgotten after this
    uint16_t port = 0;             // our port, sent with announce_peer (implied_port wins)
};

struct DhtCounters {
    uint64_t queriesSent = 0, responses = 0, errors = 0, timeouts = 0, queriesAnswered = 0;
    uint64_t malformed = 0, announcesStored = 0, announcesRefused = 0, throttled = 0;
};

struct DhtFound {
    NodeId key;
    Address peer;
};

class Dht {
public:
    using Send = std::function<bool(const Address&, const uint8_t*, size_t)>;

    explicit Dht(Send send, const DhtConfig& config = {}, const NodeId* id = nullptr);
    ~Dht();
    Dht(const Dht&) = delete;
    Dht& operator=(const Dht&) = delete;

    // A node to start from (a bootstrap router, a node remembered from last time).
    void addNode(const Address& node);
    void receive(const Address& from, const uint8_t* data, size_t size);
    void poll(double now);

    // Looks `key` up, announces us under it when `announce`, and asks the
    // closest nodes again every `refreshSeconds`; peers appear in takeFound().
    void watch(const NodeId& key, bool announce, double refreshSeconds = 5.0);
    void unwatch(const NodeId& key);
    std::vector<DhtFound> takeFound();

    const NodeId& id() const;
    size_t nodeCount() const;
    // Good nodes: answered within 15 minutes and not failing.
    size_t goodNodes() const;
    // Enough of the network known to look anything up.
    bool ready() const;
    // The address most responders saw us at (BEP 42); invalid until then.
    Address externalAddress() const;
    // Nodes worth remembering for the next start, best first.
    std::vector<Address> nodes(size_t max) const;
    const DhtCounters& counters() const;

    // The 20-byte key for a byte string: the first 20 bytes of its SHA-256.
    static NodeId keyOf(const void* data, size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saida::p2p
