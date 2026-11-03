// Meeting without a server: peers that join the same Topic find each other
// and connect, over the Internet through the Mainline DHT and on the local
// network through multicast beacons.
//
//   Topic         32 bytes every participant derives the same way. A game
//                 names its own topics: R1World a map cell, a fighting game
//                 an arena code (arenaCode / arenaTopic below).
//   discovery     the DHT stores, under a key derived from the topic, the
//                 public address of each participant's socket; every
//                 participant announces itself and keeps asking, so everyone
//                 finds everyone within `refreshSeconds` of arriving.
//   connection    both sides of a pair find each other and connect at about
//                 the same time, which is what opens their NATs; peers that
//                 already met introduce newcomers to the rest, so a crowd
//                 forms in one round trip instead of a DHT refresh.
//   admission     a handshake must carry the tag of a topic we are in (a
//                 hash; the topic itself never crosses the wire), so the
//                 address alone gets nobody in.
//   membership    each peer tells its peers which topics it is in; PeerJoined
//                 and PeerLeft follow from both sides' lists. A peer that
//                 shares no topic any more is disconnected after
//                 `lingerSeconds`.
//   proximity     a crowd is not a full mesh. With `neighbours` > 0 each peer
//                 says where it stands (setPosition), tells its peers who
//                 stands near them, asks for introductions to the nearer
//                 ones and keeps links to its `neighbours` nearest; a link
//                 either side still wants is kept, the others are dropped.
//                 Whoever arrives near you is found through the peers you
//                 already have (a Voronoi-style overlay), so there is no edge
//                 between cells: the DHT is only the door.
//   relay         two peers introduced to each other that have not linked
//                 within `relayAfterSeconds` link through the peer that
//                 introduced them (Host::relayAddress); the lower id asks
//                 first, the higher one twice as late.
//   the DHT       a topic with members needs the DHT only to heal a split:
//                 it is asked every `populatedRefreshSeconds`, and only the
//                 `announcers` members nearest the topic's key announce, so
//                 a crowded cell costs the public DHT what a quiet one does.
//
// The Rendezvous owns the DHT and the bootstrap resolution (on its own
// thread: name lookups block). It installs the Host's foreign handler,
// admission policy and service 1.
#pragma once

#include "saida/p2p/Dht.hpp"
#include "saida/p2p/Host.hpp"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace saida::p2p {

struct Topic {
    std::array<uint8_t, 32> key{};
    // SHA-256 of the application and the name: the same topic everywhere.
    static Topic named(std::string_view application, std::string_view name);
    NodeId dhtKey() const;
    std::array<uint8_t, 8> tag() const;
    bool operator==(const Topic& o) const { return key == o.key; }
    bool operator!=(const Topic& o) const { return key != o.key; }
};

// Arena codes: 40 random bits as eight Crockford base-32 characters,
// "K7QM-2XPA". Parsing ignores case, spaces and dashes, and reads O as 0 and
// I or L as 1, so a code read aloud or typed on a pad still works.
std::string newArenaCode();
bool normalizeArenaCode(std::string_view typed, std::string& canonical);
Topic arenaTopic(std::string_view application, std::string_view canonicalCode);

struct RendezvousConfig {
    bool useDht = true;
    // Resolved on the Rendezvous's thread at start. Any well-known Mainline
    // DHT routers do; nodes remembered from a previous run (knownNodes) skip
    // the names entirely.
    std::vector<std::string> bootstrap = {"router.bittorrent.com:6881", "dht.transmissionbt.com:6881",
                                          "router.utorrent.com:6881", "dht.libtorrent.org:25401"};
    std::vector<Address> knownNodes;
    DhtConfig dht;
    double refreshSeconds = 3.0;   // DHT re-query of a joined topic
    double retrySeconds = 4.0;     // a found address not reached is tried again after this
    double lingerSeconds = 10.0;   // < 0: never disconnect a peer that left our topics
    bool introduce = true;
    size_t maxPeersPerTopic = 32;  // a full mesh's bound (neighbours == 0)
    // Proximity (see the header): links kept to the `neighbours` nearest, in
    // whatever metric frame the application positions its peers (metres).
    size_t neighbours = 0;            // 0: every member links to every other
    double positionSeconds = 2.0;     // where we stand, told to our peers this often (and every 25 m)
    double gossipSeconds = 5.0;       // who stands near them, told this often (a newcomer: at once)
    size_t gossipEntries = 8;         // peers named per gossip
    double pruneSeconds = 3.0;        // a link neither side wants lasts this long
    double introductionsPerSecond = 4;// asked of our peers
    // The DHT once a topic has members: asked this often, announced by so many.
    double populatedRefreshSeconds = 60.0;
    size_t announcers = 8;
    double healSeconds = 20.0;        // a DHT address unknown to the crowd is tried this often
    bool relay = true;                // link through the introducer when punching fails
    double relayAfterSeconds = 3.0;
    // LAN discovery: beacons on an IPv4 multicast group (a separate socket).
    Address lanGroup = Address::v4(239, 255, 83, 80, 47474);
    double lanSeconds = 1.0;
};

enum class RendezvousEventType : uint8_t {
    PeerJoined,  // `peer` is in `topic` with us
    PeerLeft,    // it left the topic, or went away
};

struct RendezvousEvent {
    RendezvousEventType type;
    Topic topic;
    PeerHandle peer;
};

struct RendezvousStatus {
    size_t dhtNodes = 0, dhtGoodNodes = 0;
    bool dhtReady = false;
    Address publicAddress;
    uint64_t found = 0, attempts = 0, lanBeaconsSent = 0, lanBeaconsHeard = 0;
    // Proximity: peers heard of and not linked, introductions asked, links dropped.
    size_t candidates = 0;
    uint64_t introductionsAsked = 0, introductionsMade = 0, pruned = 0;
    size_t announcing = 0;  // topics we announce in the DHT
    uint64_t relayed = 0;   // links asked through a relay
    std::string problem;  // empty, or why nobody can be found (show it to the player)
};

class Rendezvous {
public:
    Rendezvous(Host& host, const RendezvousConfig& config = {});
    ~Rendezvous();
    Rendezvous(const Rendezvous&) = delete;
    Rendezvous& operator=(const Rendezvous&) = delete;

    // LAN beacons go through `socket`: open it with openLanSocket (an IPv4
    // socket on the group's port, shared by every game on this machine).
    void setLanSocket(DatagramSocket* socket);

    void join(const Topic& topic);
    void leave(const Topic& topic);
    bool joined(const Topic& topic) const;
    // Peers in a topic with us.
    std::vector<PeerHandle> peers(const Topic& topic) const;
    // Where we stand, in the application's metric frame (proximity only).
    void setPosition(double x, double y, double z);

    // Call after Host::poll.
    void poll(double now);
    std::vector<RendezvousEvent> takeEvents();

    // Admission for handshakes that are not the Rendezvous's own.
    void setAdmission(Host::Admission fallback);

    Dht* dht();
    RendezvousStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class UdpSocket;
// Opens `socket` for LAN beacons: IPv4, address reuse, the group's port,
// group membership. False with socket.error() when the system refuses.
bool openLanSocket(UdpSocket& socket, const RendezvousConfig& config = {});

} // namespace saida::p2p
