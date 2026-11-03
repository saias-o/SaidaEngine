// Saida netcode: deterministic rollback sessions over any datagram transport,
// and state replication for objects one machine owns.
//
//   Wire.hpp        identifiers, byte codec, state hash, input run codec
//   Simulation.hpp  the game's side: frame context, Simulation, effect ledger
//   Clock.hpp       fixed-step clock with time scale, correction smoother
//   Session.hpp     the rollback session: synchronized start, adaptive delay,
//                   prediction, rollback, membership, loss, desync and resync
//   Replay.hpp      confirmed-frame recording and verified playback
//   Replica.hpp     owner snapshots drawn elsewhere: remote clock, adaptive
//                   interpolation delay, bounded extrapolation, owner history
//   SyncTest.hpp    determinism qualification of a Simulation
//   SimNetwork.hpp  an in-process lossy network for tests and benches
#pragma once

#include "saida/netcode/Clock.hpp"
#include "saida/netcode/Replay.hpp"
#include "saida/netcode/Replica.hpp"
#include "saida/netcode/Session.hpp"
#include "saida/netcode/SimNetwork.hpp"
#include "saida/netcode/Simulation.hpp"
#include "saida/netcode/SyncTest.hpp"
#include "saida/netcode/Wire.hpp"
