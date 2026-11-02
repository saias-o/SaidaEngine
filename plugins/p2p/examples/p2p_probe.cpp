// The live check: this machine on the real Mainline DHT, and two players
// meeting by arena code with no server.
//
//   p2p_probe                 joins the DHT, reports the nodes and the public address
//   p2p_probe --new           creates an arena, prints its code, waits for players
//   p2p_probe --join CODE     joins that arena
//   options: --seconds N (default 60), --no-lan, --no-dht, --port P
//
// Run --new on one machine and --join on another (or on the same one, where
// the LAN beacons connect them). Each connected peer gets a ping every second
// and prints its round trip.
#include "saida/p2p/Rendezvous.hpp"
#include "saida/p2p/Socket.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace saida::p2p;

static double clockNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // lines appear as they happen, even in a log
    std::string code;
    bool create = false, lanOn = true, dhtOn = true;
    double seconds = 60;
    uint16_t port = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--new") create = true;
        else if (a == "--join" && i + 1 < argc) code = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--port" && i + 1 < argc) port = uint16_t(std::atoi(argv[++i]));
        else if (a == "--no-lan") lanOn = false;
        else if (a == "--no-dht") dhtOn = false;
        else return std::printf("unknown option %s\n", a.c_str()), 2;
    }
    if (create) code = newArenaCode();
    std::string canonical;
    if (!code.empty() && !normalizeArenaCode(code, canonical))
        return std::printf("'%s' is not an arena code (mistyped?)\n", code.c_str()), 2;

    UdpSocket socket;
    SocketOptions so;
    so.port = port;
    if (!socket.open(so)) return std::printf("socket: %s\n", socket.error().c_str()), 1;
    HostConfig hc;
    hc.application = "saida-p2p-probe";
    Host host(socket, Identity::generate(), hc);
    RendezvousConfig rc;
    rc.useDht = dhtOn;
    Rendezvous meet(host, rc);
    UdpSocket lan;
    if (lanOn) {
        if (openLanSocket(lan, rc)) meet.setLanSocket(&lan);
        else std::printf("LAN beacons off: %s\n", lan.error().c_str());
    }
    std::printf("socket %s, identity %016llx\n", socket.local().toString().c_str(),
                (unsigned long long)host.identity().id());
    if (!canonical.empty()) {
        meet.join(arenaTopic(hc.application, canonical));
        std::printf("%s arena %s\n", create ? "created" : "joining", canonical.c_str());
    }

    const double start = clockNow();
    double lastReport = start, lastPing = start, readyAt = -1;
    uint64_t foundBefore = 0;
    host.setReceiver([&](PeerHandle p, const uint8_t* d, size_t n, Channel) {
        if (n == 9 && d[0] == 'P') {  // a ping: echo it
            Bytes echo(d, d + n);
            echo[0] = 'E';
            host.send(p, echo, Channel::Unreliable);
        } else if (n == 9 && d[0] == 'E') {
            double sent;
            std::memcpy(&sent, d + 1, 8);
            const PeerInfo* i = host.peer(p);
            std::printf("  peer %u %s: ping %.1f ms (link rtt %.1f, loss %.0f%%)\n", p,
                        i ? i->address.toString().c_str() : "?", (clockNow() - sent) * 1000, i ? i->rtt * 1000 : 0,
                        i ? i->loss * 100 : 0);
        }
    });
    while (clockNow() - start < seconds) {
        const double now = clockNow();
        host.poll(now);
        meet.poll(now);
        for (const HostEvent& e : host.takeEvents())
            std::printf("[%5.1f s] host: %s %s%s%s\n", now - start,
                        e.type == HostEventType::Connected      ? "connected to"
                        : e.type == HostEventType::Disconnected ? "disconnected from"
                        : e.type == HostEventType::Introduced   ? "introduced to"
                                                                : "could not connect to",
                        e.address.toString().c_str(), e.reason != Reason::None ? ": " : "",
                        e.reason != Reason::None ? reasonName(e.reason) : "");
        for (const RendezvousEvent& e : meet.takeEvents())
            std::printf("[%5.1f s] arena: peer %u %s\n", now - start, e.peer,
                        e.type == RendezvousEventType::PeerJoined ? "joined" : "left");
        const RendezvousStatus st = meet.status();
        if (st.found > foundBefore) {
            std::printf("[%5.1f s] DHT: %llu address(es) announced in the arena\n", now - start,
                        (unsigned long long)(st.found - foundBefore));
            foundBefore = st.found;
        }
        if (readyAt < 0 && st.dhtReady) {
            readyAt = now;
            std::printf("[%5.1f s] DHT ready: %zu nodes, public address %s\n", now - start, st.dhtNodes,
                        st.publicAddress.toString().c_str());
        }
        if (now - lastPing >= 1) {
            lastPing = now;
            uint8_t ping[9] = {'P'};
            std::memcpy(ping + 1, &now, 8);
            for (PeerHandle p : host.peers()) host.send(p, ping, sizeof ping, Channel::Unreliable);
        }
        if (now - lastReport >= 5) {
            lastReport = now;
            const DhtCounters c = meet.dht() ? meet.dht()->counters() : DhtCounters{};
            std::printf("[%5.1f s] DHT %zu nodes (%zu good), %llu queries, %llu answers, %llu timeouts; found %llu, "
                        "tried %llu; LAN %llu/%llu%s%s\n",
                        now - start, st.dhtNodes, st.dhtGoodNodes, (unsigned long long)c.queriesSent,
                        (unsigned long long)c.responses, (unsigned long long)c.timeouts, (unsigned long long)st.found,
                        (unsigned long long)st.attempts, (unsigned long long)st.lanBeaconsSent,
                        (unsigned long long)st.lanBeaconsHeard, st.problem.empty() ? "" : "; PROBLEM: ",
                        st.problem.c_str());
        }
        host.flush();
        // A game polls once per frame; this loop polls about every millisecond
        // (Windows rounds short sleeps up to its timer period).
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    const RendezvousStatus st = meet.status();
    std::printf("done: %zu peers, DHT %s, public address %s\n", host.peers().size(), st.dhtReady ? "ready" : "NOT ready",
                st.publicAddress.toString().c_str());
    return 0;
}
