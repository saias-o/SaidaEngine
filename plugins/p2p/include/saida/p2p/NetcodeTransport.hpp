// plugins/netcode's Transport over a Host: a rollback session between peers
// that found each other with no server. The netcode PlayerId of a peer is its
// identity (PeerInfo::id), the netcode PeerId is its PeerHandle.
#pragma once

#include "saida/netcode/Session.hpp"
#include "saida/p2p/Host.hpp"

#include <algorithm>
#include <type_traits>

namespace saida::p2p {

static_assert(std::is_same_v<netcode::PeerId, PeerHandle>, "a PeerHandle is netcode's PeerId");

class NetcodeTransport final : public netcode::Transport {
public:
    explicit NetcodeTransport(Host& host) : host_(host) {}

    void send(netcode::PeerId to, const uint8_t* data, size_t size, netcode::Delivery delivery) override {
        host_.send(to, data, size, delivery == netcode::Delivery::Reliable ? Channel::Reliable : Channel::Unreliable);
    }

    // The host's game messages go to `session` (one session per host).
    void attach(netcode::Session& session) {
        host_.setReceiver([&session, this](PeerHandle from, const uint8_t* data, size_t size, Channel) {
            session.receive(from, data, size, host_.now());
        });
    }
    // Binds a connected peer under its identity; returns that PlayerId (0: unknown peer).
    netcode::PlayerId bind(netcode::Session& session, PeerHandle peer) const {
        const PeerInfo* info = host_.peer(peer);
        if (!info) return 0;
        session.bindPeer(info->id, peer);
        return info->id;
    }
    // Keeps every netcode message within one datagram.
    static void fit(netcode::Config& config, const Host& host) {
        config.maxMessageBytes = std::min(config.maxMessageBytes, host.singleDatagramMessage());
    }

private:
    Host& host_;
};

} // namespace saida::p2p
