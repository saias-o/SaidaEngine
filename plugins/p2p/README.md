# Saida P2P

`plugins/p2p` is an optional C++17 add-on that lets players meet and play
with **no server of the game's own**: encrypted peer links over one UDP
socket, NAT hole punching, and discovery through BitTorrent's Mainline DHT
(millions of nodes run by nobody in particular) and multicast beacons on the
local network. It is the transport under `plugins/netcode`, and works for any
other game protocol as well.

Two uses it was built for:

- **meeting whoever is here**: an open world joins one topic per map cell;
  players in the same cell find each other and connect, players elsewhere
  never do;
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
| `Host.hpp` | `Host`: handshakes, encrypted packets, unreliable and reliable channels, fragmentation, acknowledgements, round trip and loss, keepalive and timeout, hole punching, introductions, the public address. |
| `Bencode.hpp`, `Dht.hpp` | `Dht`: a Mainline DHT node (BEP 5, 42, 43); `watch(key)` announces us under a key and finds whoever else is there. |
| `Rendezvous.hpp` | `Rendezvous`: `Topic`s, arena codes, DHT and LAN discovery, admission by topic, membership, introductions, `problem` when nobody can be found. |
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
`lingerSeconds`. What a cell is belongs to the game.

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
asks the closest nodes again every `refreshSeconds`; both sides of a pair find
each other and connect. A newcomer that reaches one member of a topic is
introduced to all the others, so a crowd forms in one round trip. Handshakes
carry the topic's tag (a hash; the topic itself never crosses the wire) and
are refused without one. On the local network, beacons on a multicast group
carry the tags and the game port.

**Refusals are said.** Every refused datagram or send is counted under a named
`Reason`; a failed `connect()` is a `ConnectFailed` event with its reason; a
mistyped arena code is refused by its check character instead of searching
for an arena that does not exist; `RendezvousStatus::problem` names an
unreachable DHT.

## Proven

`tests/test_p2p.cpp` (`PASS saida::p2p`; 0–2 also under `_GLIBCXX_ASSERTIONS`
and UBSan), on a Ryzen 7 5700U. Over 240 runs of seeds 0–179, three failed and
none of them again on the same seed (keys and indices are truly random): twice
the 8-player cell mesh was not complete within 20 s, once two DHT clients
found nothing within 20 s. Open in ROADMAP 5.

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
| LAN beacons, two processes on one machine | met in about 45 ms |

Live, on the real Mainline DHT from a home connection (`examples/p2p_probe.cpp`,
2 November 2026): ready in 0.2 s with about 55 nodes, the public address
learned; two processes on one arena code found each other's announcement
0.5 s and 2 s after joining; connected over the LAN beacons in 0.1 s.

## Limits

- A symmetric NAT on one side and a port-restricted or symmetric NAT on the
  other cannot be punched: those pairs need a relay, which does not exist yet.
- Two players behind the same router meet through the LAN beacons; the DHT
  path needs the router to hairpin, which many home routers do not.
- The DHT is IPv4 only (BEP 32 is not implemented); links themselves work over
  IPv6.
- No key rotation within a session and no congestion control beyond the
  in-flight and per-flush bounds: links are sized for game traffic.
- The Web runtime has no raw UDP; this add-on is native only.
- Bootstrap uses public DHT routers by name; `Dht::nodes()` saved by the game
  and given back as `knownNodes` skips them next time.
