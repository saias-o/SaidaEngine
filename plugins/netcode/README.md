# Saida Netcode

`plugins/netcode` is an optional, header-only C++17 add-on for deterministic
rollback multiplayer, in the family of GGPO and of the netcode of modern
fighting games: every participant simulates everyone, local input is applied
after a delay chosen from the line, remote input is predicted, and a wrong
prediction is corrected by loading an earlier state and simulating forward
again. There is no server and no permanent arbiter.

It is not linked into `saida_engine`, it is absent from CMake, and it is never
loaded by the editor, the runtime or an exported player. A project that
includes the headers compiles them into its own binary. It uses the C++
standard library only: no scene, node, socket, thread, clock or allocator of
its own.

## What it is

| Header | Responsibility |
| --- | --- |
| `Wire.hpp` | `Frame`, `PlayerId`, `PeerId`; little-endian `Writer`/`Reader` that fail instead of overreading; `hash64`; the XOR + run-length input codec. |
| `Simulation.hpp` | The game's side: `FrameContext`, `Simulation` (save, load, advance, predict, commit, retract) and the `Ledger` of effects and commits. |
| `Clock.hpp` | `TickClock`: frame time to whole fixed ticks, with the session's time scale and bounded catch-up. `Smoother<N>`: hides a rollback correction on screen. |
| `Session.hpp` | `Session`: synchronized start, adaptive input delay, prediction, rollback, time sync, membership, loss, desync detection and resync, connection quality. `Transport` is what the host implements underneath. |
| `Replay.hpp` | `Replay` records confirmed frames; `ReplayPlayer` plays them back and checks every recorded state hash. |
| `SyncTest.hpp` | Qualifies a `Simulation` for rollback: replays the last frames every frame and names the first one whose state differs. |
| `SimNetwork.hpp` | An in-process network with latency, jitter, loss, duplication and reordering, implementing `Transport`. |

`Netcode.hpp` includes them all.

## Use

```cpp
#include "saida/netcode/Netcode.hpp"
using namespace saida::netcode;

class Game : public Simulation {
    void save(Frame, std::vector<uint8_t>& out) override;        // every simulated byte
    void load(Frame, const uint8_t* data, size_t size) override;
    void advance(const FrameContext& f) override {
        for (size_t i = 0; i < f.departedCount; ++i) despawn(f.departed[i]);
        for (size_t i = 0; i < f.playerCount; ++i) {
            const auto in = f.input<PadState>(i);                // sorted by player id
            if (f.players[i].joined) spawn(f.players[i]);
            if (fire(f.players[i], in) && f.effect(key)) playSound();   // never twice
            if (scored) f.commit(key, &score, sizeof score);        // once, when confirmed
        }
    }
    void predict(PlayerId, int ahead, const uint8_t* last, uint8_t* out, size_t n) override {
        std::memcpy(out, last, n);                               // directions held...
        reinterpret_cast<PadState*>(out)->pressed = 0;           // ...presses never invented
    }
    void commit(Frame, uint64_t key, const uint8_t* data, size_t size) override; // persist
    void retract(Frame, uint64_t key) override;                  // a corrected-away effect
};

Config cfg;
cfg.sessionId = id;            // the founder's choice
cfg.inputSize = sizeof(PadState);
Session session(cfg, myPlayerId, game, transport);
session.bindPeer(otherId, otherPeer);           // the transport's authenticated endpoints
session.start({myPlayerId, otherId}, now);      // or startAlone(now), or join(sponsor, now)

TickClock clock(cfg.tickSeconds);
// each rendered frame
for (auto& packet : received) session.receive(packet.from, packet.data, packet.size, now);
session.poll(now);
for (const Event& e : session.takeEvents())
    if (e.type == EventType::Started) clock.reset(session.startLateness());
for (int n = clock.advance(frameSeconds, session.timeScale()); n > 0; --n)
    session.tick(&pad);         // Advanced, Waiting(player), Skipped, Synchronizing...
render(clock.alpha());
drawBars(session.quality().bars);
```

The simulation must be a pure function of its saved state and the frame
context: no clock, no unseeded random, no state outside `save()`, no
local-only data, the same bytes on every machine. Run `SyncTest` on it before
any network; it fails on the first frame that breaks this, and says which.

## Model

- **Start.** Founders ping each other every 50 ms until each has
  `syncSamples` round trips (at most `syncSeconds`). The coordinator then
  sends each founder how long to wait, its own lead minus that founder's
  one-way time, so frame 0 begins at the same instant on every machine
  (`Started`; carry `startLateness()` into the clock). Three founders on
  20, 45 and 70 ms lines began within the same rendered frame, 0.47 s after
  `start`.
- **Input delay.** By default the delay follows the line, like a fighting
  game's automatic setting: the one-way time to the farthest member plus
  twice its jitter (RFC 3550, between successive round trips), in frames,
  minus `rollbackComfort` (3), within `minInputDelay`..`maxInputDelay`
  (1..6). The target is the worst seen over each second, so it does not
  flicker. It moves one frame per tick and never loses or invents input:
  raising repeats the last input over the frame skipped; lowering waits for a
  tick whose input equals the last one sent (or gives up one sample after
  half a second of constant change). `setInputDelay` fixes it instead.
  Changing it is local; nobody else needs to know.
- **Inputs.** Each participant's input is `inputSize` bytes per frame, sent to
  every peer at once with every frame the peer has not acknowledged (up to
  128, oldest first, within `maxMessageBytes`). A held stick costs about two
  bytes for any number of frames. Missing remote input is predicted by
  `Simulation::predict` (default: repeat) for at most `maxPrediction` frames;
  past that, `tick` returns `Waiting` with the player it waits for.
- **Rollback.** A received input that differs from the one used, or a roster
  change for a frame already simulated, rewinds to that frame: its saved state
  is loaded and every frame since is simulated again (`Pass::Resimulate`). A
  rollback is never deeper than the prediction window. A state is saved only
  if a rollback may return to it: a frame whose every input is already known
  when it is simulated is not saved (except checksum frames and while a
  snapshot is owed).
- **Time.** Members exchange their frame and measured advantage;
  `timeScale()` stretches the fixed step by up to `maxTimeAdjust` so drifting
  clocks converge without stalls. A machine more than `skipAdvantage` frames
  ahead (the other side hitched) gives back a tick (`Skipped`, at most one
  per `skipInterval`) instead of making the others roll back further: after a
  300 ms freeze on one machine, the two were back in step within a second.
- **Effects and commits.** `FrameContext::effect(key)` is true the first time a
  frame fires a key; a resimulation that fires it again gets false, one that
  no longer fires it calls `retract`. Nothing fires while catching up.
  `FrameContext::commit` records a consequence that `Simulation::commit`
  receives exactly once, when its frame is confirmed, with the bytes of the
  frame's last simulation.
- **Membership is input.** Admitting a newcomer, making it live, leaving and
  dropping a lost player are commands written into the writer's own input for
  a frame, so every machine applies them at the same frame and the roster is
  as deterministic as the inputs. Competing commands about one player are
  ordered by (frame, sponsor id). `FrameContext` lists exactly the players
  taking part in a frame, `joined` on their first, and who departed.
- **Joining.** The newcomer sends `JoinRequest` to a member it is bound to (the
  sponsor). The sponsor writes `Add`, waits for that frame to be confirmed,
  and sends a confirmed snapshot (state, roster, each player's last input) in
  reliable chunks. The newcomer loads it, receives every member's input from
  that frame, catches up `maxCatchUpFrames` per tick, proposes a first frame,
  and goes live at the frame the sponsor writes (`Live`). Any member may
  sponsor, including one that has just joined. A full session, another input
  size or a host refusal is answered with a named reason.
- **Leaving.** `leave()` writes the local player's last frame into its input,
  then lingers until the members acknowledge it.
- **Loss.** A member silent past `interruptSeconds` is reported `Interrupted`;
  past `disconnectSeconds` it is lost. The loss is broadcast with the last
  frame each member holds from it; the lowest-id member standing (the
  *coordinator*, recomputed from the roster, never permanent) writes the drop
  at the latest of those frames, so no member ever confirmed a frame the drop
  rewrites. Members missing some of those frames, or any input of a member
  gone quiet past `relayAfterSeconds`, fetch them from the others (input
  relay): a cut between two members is bridged by a third. Every member must
  reach every other; one that cannot is dropped by all, and its packets are
  refused afterwards (`SenderLost`, `StaleIncarnation`). A drop or leave
  written while the session is waiting rides on one extra frame of the last
  input, so it never waits for the frames it unblocks.
- **Desync.** Every `checksumInterval` frames each member hashes its confirmed
  state and publishes the hash with its inputs. A mismatch is a `Desync`
  event carrying both hashes; with the coordinator, the session (with
  `autoResync`) asks for the coordinator's confirmed state, loads it and
  simulates forward. Frames already committed are not committed again.
- **Quality.** `quality()` gives the worst member's round trip, jitter and
  loss (smoothed per second), last second's resimulated frames, deepest
  rollback and stalls, the current delay and advantage, and 0-5 bars.
- **Replays.** `record(&replay)` writes every confirmed frame from the next
  checksum frame: roster changes, departures and XOR-coded inputs (a 3-4
  player match: about 4 bytes a frame), plus the state hashes. `bytes()` seals
  it with a hash; `parse` refuses anything damaged or truncated.
  `ReplayPlayer` plays it through a simulation, firing effects, committing
  only if asked, and stops at the first frame whose state hash differs (a
  changed simulation, a desync to reproduce). A resync restarts the recording.

## Refusals are said

Every refused packet is counted in `Counters::rejected` under a `Reason` and
reported as a `Rejected` event (at most once per reason per second);
`reasonName` names it. A failed join, a kick and a departure end with
`JoinFailed`/`Left` and their reason. Nothing is refused silently.

## Trust and limits

- The transport authenticates who a `PeerId` is and protects message
  integrity; the session refuses a packet whose claimed sender is bound to
  another endpoint (`Impersonation`), a stale incarnation, a lost sender, a
  wrong session or protocol, malformed or oversized content, frames outside
  the input window, and senders over `receiveBudget` bytes per second. It
  survives arbitrary bytes, but the content of an authenticated member is
  trusted: inputs are not validated against game rules.
- Reliable messages (control, snapshots, start) need ordered, lossless
  delivery per peer from the transport; inputs and pings use unreliable
  datagrams.
- A full mesh: N players exchange N-1 streams each. A session freezes
  (`Waiting`) while a member is silent, up to `disconnectSeconds` before the
  drop. A partition splits into sessions that refuse each other.
- On a desync, the coordinator's state wins, whichever was right.
- Hashes read words little-endian (every Saida target).
- There is no socket, discovery, NAT traversal or encryption here: that is the
  transport's job.

## Measures

`tests/test_netcode.cpp` drives full sessions over `SimNetwork`, polled once
per 60 Hz rendered frame (so round trips include up to two frames of polling,
as in a game). On the reference laptop (Ryzen 7 5700U), a toy arena
simulation, adaptive delay, `maxPrediction` 8.

20 s, 50-70 ms one way, 5% loss, 1% duplication:

| Players | Session + arena CPU per machine per frame | Frames resimulated per frame | Upload per machine | Deepest rollback |
| --- | --- | --- | --- | --- |
| 2 | 0.001 ms | 0.11 | 2.8 KB/s (67 msg/s) | 4 |
| 4 | 0.002 ms | 0.21 | 8.5 KB/s | 4 |
| 8 | 0.006 ms | 0.36 | 19.9 KB/s | 5 |
| 16 | 0.013 ms | 0.36 | 42.8 KB/s | 5 |

Every machine agreed on every hashed frame. Two players, 12 s, 4 ms jitter,
no loss:

| One way | Round trip seen | Delay chosen | Deepest rollback (last s) | States saved | Bars |
| --- | --- | --- | --- | --- | --- |
| 5 ms | 33 ms | 1 | 0 | 7% | 5 |
| 20 ms | 67 ms | 1 | 1 | 92% | 4 |
| 50 ms | 133 ms | 1 | 3 | 94% | 3 |
| 80 ms | 169 ms | 4 | 2 | 92% | 2 |
| 120 ms | 267 ms | 5 | 3 | 93% | 1 |

The delay stays at its minimum until the line needs more than three frames of
rollback; rollback never exceeds `rollbackComfort`. A real game's cost is
dominated by its own `save`, `load` and `advance`, multiplied by the frames
resimulated and the states saved.

## Verification

One translation unit, no engine build:

```sh
cd plugins/netcode/tests
g++ -std=c++17 -O2 -Wall -Wextra -I../include test_netcode.cpp -o test_netcode && ./test_netcode
```

It prints `PASS saida::netcode`. `./test_netcode <n>` replays every bench with
other network draws; seeds 0-199 pass, and 0-39 also under
`-D_GLIBCXX_ASSERTIONS -fsanitize=undefined`. Covered: codec round-trips and
garbage, the clock, the smoother, the ledger, `SyncTest` on a sound and an
unsound simulation, two players on a clean line, four over a lossy,
reordering, duplicating line with drifting clocks (agreement, rollbacks, no
effect shown twice, identical commits, effects left on screen equal to the
commits), joining alone then one by one with a 150 KB snapshot, a newcomer
sponsoring a newcomer, leaving, rejoining as a new incarnation, the abrupt loss
of a member and of the coordinator, a cut that stalls and resumes, a cut
bridged by relay, a diverging machine detected and resynchronized, 20 000
hostile packets, impersonation, a full session, a mismatched build, a
synchronized start on unequal lines, the delay rising on a long line and
falling on a short one, a fixed delay, a 300 ms hitch, a game predictor, the
quality bars, and a recorded match with a join and a leave played back with
every hash and commit, a changed simulation caught, and damaged replays
refused.
