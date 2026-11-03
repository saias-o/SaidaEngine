# Saida P2P

`plugins/p2p` is an optional C++17 add-on that lets players meet and play
with **no server of the game's own**: encrypted peer links over one UDP
socket, NAT hole punching, and discovery through BitTorrent's Mainline DHT
(millions of nodes run by nobody in particular) and multicast beacons on the
local network. It is the transport under `plugins/netcode`, and works for any
other game protocol as well.

Two uses it was built for:

- **meeting whoever is here**: an open world joins one topic per map cell;
  players in the same cell find each other, and in a crowd each links only to
  the players nearest it (proximity), found through the peers it already has,
  so a crowd of any size costs each player a bounded number of links and has
  no edge at a cell's border;
- **arenas by code**: one player creates an arena and reads out `K7QM-2XPA`,
  the other types it (any case, with or without the dash); they are connected
  within seconds and play rollback netcode peer to peer.

It is not linked into `saida_engine` and is never loaded by the editor, the
runtime or an exported player. A game adds the directory with CMake and links
`Saida::P2P`. It uses the engine's mbedTLS for cryptography and the system's
sockets; `netcode`'s `Wire.hpp` for its byte encoding.

## What it is

| Header | Responsibility |
| --- | --- |
| `Address.hpp` | `Address` (IPv4/IPv6 endpoint, one spelling per endpoint, compact form) and `DatagramSocket`, the interface every layer sends through. |
| `Socket.hpp` | `UdpSocket`: non-blocking, dual-stack, the Windows ICMP-reset trap disabled, IPv4 multicast for LAN beacons; `resolve` (blocking). |
| `Crypto.hpp` | X25519, ChaCha20-Poly1305, SHA-256, HMAC on mbedTLS; `NoiseXX`, the Noise XX handshake. |
| `Host.hpp` | `Host`: handshakes, encrypted packets, unreliable and reliable channels, fragmentation, acknowledgements, round trip and loss, keepalive and timeout, hole punching, introductions, the public address, relaying for peers that cannot reach each other. |
| `Bencode.hpp`, `Dht.hpp` | `Dht`: a Mainline DHT node (BEP 5, 42, 43, and 32 for IPv6); `watch(key)` announces us under a key and finds whoever else is there. |
| `Rendezvous.hpp` | `Rendezvous`: `Topic`s, arena codes, DHT and LAN discovery, admission by topic, membership, introductions, proximity (links to the nearest), relay fallback, `problem` when nobody can be found. |
| `NetworkThread.hpp` | `NetworkThread`: a Host and its Rendezvous on their own thread, woken by datagrams, with a fixed-step callback and a lock for the game. |
| `NetcodeTransport.hpp` | `plugins/netcode`'s `Transport` over a `Host`. |
| `SimInternet.hpp` | An Internet in one process: public hosts, the four kinds of NAT, lines with latency, jitter, loss and duplication. |

## Use

```cpp
#include "saida/p2p/NetcodeTransport.hpp"
#include "saida/p2p/Rendezvous.hpp"
#include "saida/p2p/Socket.hpp"
using namespace saida::p2p;

UdpSocket socket;
socket.open();                                   // any port, IPv4 and IPv6
HostConfig hc;
hc.application = "my-fighter/3";                 // other games (and versions) never connect
Host host(socket, identity, hc);                 // identity: Identity::generate(), kept on disk
Rendezvous meet(host);                           // DHT, bootstrapped by name on its own thread
UdpSocket lan;
if (openLanSocket(lan)) meet.setLanSocket(&lan); // players behind the same router

const std::string code = newArenaCode();         // show it; or normalizeArenaCode(typed, code)
meet.join(arenaTopic(hc.application, code));

// Every frame:
host.poll(now);
meet.poll(now);
for (const RendezvousEvent& e : meet.takeEvents())
    if (e.type == RendezvousEventType::PeerJoined) link.bind(session, e.peer);
session.poll(now);                               // plugins/netcode, over NetcodeTransport link(host)
... session.tick(input) ...
host.flush();                                    // everything sent this frame, coalesced
if (!meet.status().problem.empty()) show(meet.status().problem);
```

`NetcodeTransport::fit(config, host)` keeps every netcode message within one
datagram; `attach(session)` routes the host's game messages to the session.

An open world joins `Topic::named(app, "cell/" + cellName)` for the cell the
player stands in (and its neighbours, if it wants them), and leaves it on the
way out. A peer that shares no topic any more is disconnected after
`lingerSeconds`. What a cell is belongs to the game. A crowd sets
`RendezvousConfig::neighbours` and says where the player stands, in any metric
frame all its players share:

```cpp
rc.neighbours = 32;                              // links to the 32 nearest, not to everyone
Rendezvous meet(host, rc);
meet.setPosition(x, y, z);                       // every frame, or whenever he moves
```

To run the network off the frame loop:

```cpp
NetworkThread net(host, &meet, socket);          // owns host and meet once started
net.start([&](double now) { sendSnapshots(now); });  // every 1/60 s, lock held
// in the frame, around anything that touches host or meet:
auto held = net.lock();
```

## How it works

**Handshake.** Noise XX (`Noise_XX_25519_ChaChaPoly_SHA256`) in three
messages. Each side proves its static key; `PeerInfo::id`, a hash of that key,
is the participant's identity (netcode's `PlayerId`). The application name is
hashed into the handshake, so different games, or incompatible versions,
cannot complete one. The first packet carries an *intent* (the Rendezvous puts
a topic tag there): the responder's admission policy sees it before spending
any key agreement, under a rate budget. The first packet is padded so that the
answer is never larger than the request: a forged source address cannot turn
a host into an amplifier.

**Packets.** Each datagram is `type, receiver index, packet number` (9 bytes)
then ChaCha20-Poly1305 with the packet number as nonce (16-byte tag): 25 bytes
over UDP. A 256-packet window refuses duplicates and replays. Inside, frames:
acknowledgements (the largest packet and the 64 before it), unreliable
messages, reliable messages, fragments, keepalives, introductions. Everything
queued for a peer during a frame leaves at `flush()` in as few datagrams as
fit the MTU (1200 bytes by default): no IP fragmentation, ever.

**Reliability.** Reliable messages are numbered and delivered once, in order,
however the packets carrying them are lost, duplicated or reordered. A packet
is lost when three later ones are acknowledged and it is older than 9/8 of the
round trip, or past the retransmission timeout (QUIC's rules); its fragments
are sent again. A late acknowledgement turns a declared loss back into a late
packet in the statistics. Round trip and its variance follow RFC 6298, from
the handshake's own exchange onwards (Karn's rule).

**NAT traversal.** `connect()` repeats its first packet, at 0.1, 0.2, 0.4 s
and then every second: two peers that connect to each other at once open both
NATs. When the two first packets cross, each side answers the other's and
keeps its own; whichever a NAT lets through makes the link, and two that both
succeed make one peer. Every peer tells the other the address it sees, so a
host behind a NAT learns its public address. `Host::introduce` tells two peers
about each other, so they punch at the same moment.

**Discovery.** The Rendezvous announces the socket's public port in the DHT
under a key derived from the topic (`announce_peer` with `implied_port`) and
asks the closest nodes again every `refreshSeconds`, moving to any nearer node
they mention, so every watcher of a topic converges on the same nodes; both
sides of a pair find each other and connect. A DHT nobody answered asks its
bootstrap nodes again, from 2 s up to every minute. A newcomer that reaches one member of a topic is
introduced to all the others, so a crowd forms in one round trip. Handshakes
carry the topic's tag (a hash; the topic itself never crosses the wire) and
are refused without one. On the local network, beacons on a multicast group
carry the tags and the game port.

**Proximity.** With `neighbours` > 0 a peer tells its peers where it stands
(every `positionSeconds`, or after 25 m) and, every `gossipSeconds`, the
`gossipEntries` peers it knows nearest to each of them (a newcomer hears it at
once). A peer named nearer than one of its own links asks the peer that named
it for an introduction; it keeps links to its `neighbours` nearest plus a
quarter, tells each peer whether it wants the link, and drops after
`pruneSeconds` a link neither side wants. Whoever arrives near you is in the
gossip of someone already near you: the overlay follows the players (a
Voronoi-style overlay, VON), and cells only decide who may meet.

**The DHT is the door.** Once a topic has members, its DHT key is asked every
`populatedRefreshSeconds` instead of every `refreshSeconds`, and only the
`announcers` members whose ids hash nearest the key announce; an address the
DHT gives that no member has is tried every `healSeconds`, which heals a crowd
that formed in two pieces. A crowded cell then costs the public DHT what a
quiet one does.

**Relay.** A host can carry datagrams between two of its peers
(`HostConfig::relay`, within `relayBytesPerSecond`). `connect(Host::relayAddress(via, id))`
sends the handshake and every packet of that link inside frames of the link to
`via`, which hands them to the peer `id` as coming from a virtual address of
its own: the two ends run Noise end to end, so the relay reads and forges
nothing. A relayed link is never relayed again; `PeerInfo::via` names the
relay. The Rendezvous does it by itself: two peers it introduced that are not
linked after `relayAfterSeconds` link through the introducer (the lower id
asks, the higher one twice as late).

**IPv6.** The DHT is two networks on one dual-stack socket, as BEP 32 has it:
IPv4 and IPv6, each with its routing table, `nodes6` and 18-byte peers; a
watch asks both, and bootstrap names resolve to both families.

**The network thread.** `NetworkThread` waits on the socket (`select()`) until
the next step or the first datagram, polls the Host and the Rendezvous, runs
the step callback every `stepSeconds`, and flushes. Arrivals are stamped when
they land, acknowledgements and keepalives leave on time, and a game frame that
stalls for half a second stalls nothing. The game touches the Host and the
Rendezvous only under `lock()` (recursive); callbacks run on the thread with
it held.

**Refusals are said.** Every refused datagram or send is counted under a named
`Reason`; a failed `connect()` is a `ConnectFailed` event with its reason; a
mistyped arena code is refused by its check character instead of searching
for an arena that does not exist; `RendezvousStatus::problem` names an
unreachable DHT.

## Proven

`tests/test_p2p.cpp`, on a Ryzen 7 5700U: every simulated test passes on seeds
0–399, and seeds 0–9 also under `_GLIBCXX_ASSERTIONS` and UBSan with the real
sockets. `test_p2p [seed [test ...]]` makes keys, indices and network draws a
function of the seed (`seedRandomForTesting`), so a failure replays exactly.
That is how the last two rare failures were found: a DHT whose bootstrap
answers were lost never asked again, and two lookups that ended on different
nodes never met; both are fixed (bootstrap retried with backoff, refreshes
that keep walking towards the key). The proximity, crowd, relay, IPv6 DHT and
network thread tests (3 November 2026) passed with every earlier simulated
test on seeds 0–39, the 150-player crowd on seeds 0–3; the 0–399 and UBSan runs
above predate them. Seeds 6 and 21 found the one rare failure among them: an
introduction travels unreliably, so when only the higher id heard it nobody
asked for the relay; the higher id now asks too, twice as late.

| Check | Result |
| --- | --- |
| RFC 7748, FIPS 180, RFC 4231 vectors; AEAD tamper and nonce | through the wrappers |
| Noise XX | keys agree; tampered messages and another prologue refused |
| Seal and open one 100-byte packet | 1.4 µs |
| 300 KB reliable and 9 KB unreliable message | intact |
| 7,200 reliable messages, 23 % loss end to end, reordering, duplicates | all, once, in order |
| Replays, flipped bits, 5,000 random datagrams, an Init flood | refused and counted; at most the rate budget answered |
| Crash and restart of a peer with the same identity | old link `Restarted`, new one connected |
| NAT pairs, both sides punching | every pair except symmetric with port-restricted or symmetric |
| Introduction of two port-restricted peers | connected |
| DHT of 150 nodes, two clients behind NATs | each finds the other within 1–5 s |
| Arena code, port-restricted and address-restricted players | together 1–3 s after typing |
| Rollback session (netcode) over that link, 3 % loss | 1,175 frames each, 16 equal checksums, delay 1, 6 KB/s up |
| 8 players, 2 map cells, mixed NATs | a full mesh per cell in about 3 s, nothing across; a player who walks over changes mesh |
| Loopback, IPv4 and IPv6, real sockets | handshake in about 2 ms |
| Proximity: 36 players, 6 nearest each, a walker crossing the crowd | every nearest linked within a second, 9 links each instead of 35; the walker's links follow him |
| Crowd: 150 players in one cell, 32 nearest each, one in ten behind a symmetric NAT | 98 % of the nearest linked 6-9 s after the last arrived, 42 links each (a full mesh is 149), about 250 links relayed, 6 KB/s of overlay a player |
| The same crowd and the public DHT | 1.9 queries a second at the busiest node; 48 when every member refreshed every 3 s and announced |
| Relay: symmetric against port-restricted, through a public peer | linked in 0.4 s, a 120 KB reliable message intact, the relay sees no plaintext; gone with the relay |
| Two players behind symmetric NATs and a third, by the Rendezvous alone | all linked in 4-8 s |
| IPv6 DHT of 60 nodes (in memory) | two clients find each other in about 2.6 s |
| Network thread, real sockets, the caller stalled for 600 ms | 36 snapshots of 60 a second delivered meanwhile, widest gap 18 ms |
| LAN beacons, two processes on one machine | met in about 45 ms |

Live, on the real Mainline DHT from a home connection (`examples/p2p_probe.cpp`,
2 November 2026): ready in 0.2–1.8 s with 25–55 nodes, the public address
learned; two processes on one arena code found each other's announcement
0.5–4 s after joining; connected over the LAN beacons at once, pinging in
0.2 ms.

## Limits

- A symmetric NAT on one side and a port-restricted or symmetric NAT on the
  other cannot be punched: they are relayed by a peer both reach, and stay
  apart when no such peer exists (two such players alone in a topic).
- Two players behind the same router meet through the LAN beacons; the DHT
  path needs the router to hairpin, which many home routers do not.
- Proximity needs positions in one metric frame for the whole application;
  a peer that sends none is kept but never chosen as a nearest.
- No key rotation within a session and no congestion control beyond the
  in-flight and per-flush bounds: links are sized for game traffic.
- The Web runtime has no raw UDP; this add-on is native only.
- Bootstrap uses public DHT routers by name; `Dht::nodes()` saved by the game
  and given back as `knownNodes` skips them next time.
