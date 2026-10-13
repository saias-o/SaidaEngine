# Saida Traffic

`plugins/traffic` is an optional, header-only C++ add-on that drives ambient
road traffic over a lane graph. It is not linked into `saida_engine`, it is
absent from CMake, and it is never loaded by the editor, the runtime or an
exported player. A project that does not include the header is unaffected by
it; a project that does compiles it into its own binary, which is what lets a
game link against a prebuilt engine and still gain traffic.

The separation is the same one `plugins/python-tools` makes, for the same
reason: the capability is real, it is tested, and it is not part of the engine's
contract.

## What it is

`include/saida/traffic/Traffic.hpp` includes the small `Signals.hpp` timing
module and exposes these main types:

| Type | Responsibility |
| --- | --- |
| `Graph` | Nodes and directed lanes in one coordinate frame, plus the adjacency built once from them. Immutable while driven on. |
| `Rules` | How this place drives: side of the road, lane offset, acceleration, braking, headway, junction guard, spawn and despawn radii. |
| `Flow` | A population of agents on one graph. `update()` drives them; `pose()` says where one is. |

It uses the C++ standard library. **It knows nothing about
the engine** — no scene, no node, no material, no serialization, no glm, no
coordinate system beyond "two axes the ground lies in". The host owns
rendering, owns where the graph came from, and reads poses back.

That seam is what makes it testable without a window and reusable by a
hand-built city as readily as by a streamed planet.

## Model

Agents follow offset lanes, cubic junction curves and speed limits. Lane data
includes widths, strip/road identities, layers, priorities, turn masks, stop/yield
and signal assignments. Following uses vehicle length and a time gap; safe
adjacent-lane changes blend over time. Junction admission keeps the current
owner and orders arrivals, including a waiting-time safeguard against starvation.
Programs support green, amber, all-red and pedestrian phases; opposing approaches
share green by default. Set `SignalTiming::opposingTogether=false` for separate
protected approach phases. Shape points continue the road without acquiring a
junction reservation. This is conservative intersection management, not a full
movement-conflict scheduler or destination planner.

The spatial sweep caches positions and reuses sorted scratch storage for
corridor/junction queries. Nested junction queries can reach O(n³) in the dense worst case. The host should
bound active populations and configure `DriverSettings` for its vehicle sizes.

**Every distance is measured between lane positions, never between centre-line
points.** A two-way street is two lanes over one line, so a car measured on the
line has every oncoming car sitting directly in front of it, and the flow stops
dead at the first car it meets. The tests fail on exactly this.

## Use

```cpp
#include "saida/traffic/Traffic.hpp"

saida::traffic::Graph graph;                 // fill nodes and lanes, then:
graph.build();

saida::traffic::Flow flow;
flow.reset(&graph, saida::traffic::Rules{}, seed);
flow.setPopulation(14);                      // how busy this network is

// each frame
saida::traffic::Obstacle player{{x, y}, 2.2f, true};
flow.update(dt, {observerX, observerY}, {lookX, lookY}, player);
for (const auto& agent : flow.agents()) {
    if (!agent.alive) continue;              // a dead agent keeps its slot
    const auto pose = flow.pose(agent);      // position, yaw, speed
}
```

Slot indices remain stable while a trip lives; `Agent::identity` distinguishes
slot reuse. Lowering population retires only eligible offscreen trips. Hosts
that supply `setVisibility` retain visible vehicles at dead ends and during
population reduction. Do not use a changing RNG seed as vehicle identity.

For dynamic cars, call `setPhysicalPose` before `update`, then `command` to
obtain throttle, brake and steer. The solver owns the pose; the flow must not
teleport the body. An incident brakes and remains an obstacle. Call `setTime`
with shared host time so signal phases agree between streamed regions.
Use `setVehicleLimits` for each new identity to provide its acceleration,
braking, lateral acceleration, maximum speed, wheelbase and steering angle in
radians. Braking distances, corner speeds and physical steering then use that
vehicle's capabilities, including heavy buses and trucks. Zero wheelbase or
steering angle retains the flow's default geometry.
Use `setSpawnSize(callback)` to return a `VehicleSize` from `(seed, lane)` before
spawn clearance is tested; positive finite dimensions are required. Without the
callback the default car dimensions apply. `Agent::vehicleSeed` is the stable
size/appearance seed; routing advances `Agent::seed` independently. A host must render the
same vehicle size for that trip, including recycled slots.

`Lane::stopLineDistance` is a host-supplied stop setback (NaN uses the driver's
default). Route lookahead anticipates controls beyond short shape segments, so
the final segment need not fit the stop line or the car. Drivers committed at
green or unable to stop at amber finish crossing when the phase changes. Stop
sign completion remains tied to its node across intermediate segments. Trips
brake at terminal roads; completed offscreen physical trips retire just like
graph-driven trips, while visible vehicles stay available for the host's handoff.
Following also looks through bends and short continuations. Junction contention
ignores vehicles held behind their queue leader or an occupied exit. Birth
clearance uses the resolved curve pose and excludes junction mouths and stop
setbacks; a short straight continuation still permits spawning. Exclusion zones
are prepared once for the immutable graph rather than scanned from all lanes
for every spawn candidate.

`capacityNear(observer)` estimates lane room inside the spawn radius, allowing a
host to share a fleet budget without allocating it to distant tiles. Following
uses actual combined vehicle widths. Supply `Obstacle::halfLength`, `halfWidth`
and `yaw` for an oriented parked-vehicle footprint; omitted dimensions retain
the circular obstacle behavior.

A host can pool its scene nodes per slot to avoid repeated allocation and model
loading. In Saida, use `Node::setVisible(false)` between trips and restore
visibility on reuse. Hidden nodes retain their resources; visibility changes
refresh only the affected scene branches. Keep the pool scoped to its region.

`facing` is the direction the observer is looking, or `{0, 0}` when the host
does not track one. Given it, a car may appear much closer behind the observer
than in front: the hole in front is what a player sees, and the one behind is
what makes a city feel empty for no reason.

A streaming host gives each streamed region its own `Graph` and its own `Flow`.
Use `adopt` with a compatible actual pose and road identity, then `retire` the
source slot, to cross regions. Adoption selects the nearest finite segment and
resolves its continuation immediately. Physical poses retain a negative entry
distance within `transferTolerance` until the centre enters the segment. Keep
the adopted region as owner while entry completes; returning to the old region
on the centre alone repeatedly resets routing and traps queues at seams.
Move the original host node and its damage state.
Graphs must stay at stable addresses while their flows refer to them. Removing
a region without transferring its live vehicles destroys those trips.

## Verification

Neighbour queries use each physical car's actual position and oriented footprint,
including sideways incidents. Lookahead includes stopping distance for the
vehicle's own braking capability. A stationary driver retains brake input,
and spawning checks clearance behind the candidate as well as ahead. Updates
accept up to 0.25 s and subdivide into steps of at most 0.05 s, so low frame rates
do not silently slow the simulation.

```sh
cd plugins/traffic/tests
g++ -std=c++20 -O2 -Wall -I../include test_traffic.cpp -o test_traffic
./test_traffic
```

One translation unit, standard library only, no engine build required. It
prints `PASS saida::traffic` or names the property that broke. The cases are
properties a street must have for a player at ground level not to notice the
simulation: cars stay on their lane, on the correct side; they keep a gap; they
do not brake for oncoming traffic; they do not drive through the player; a
crossroads does not deadlock or overlap; a dead end retires a car; and one seed
is one street on every run.

## Limits

- Junction reservations serialize conflicting traffic conservatively; they
  do not implement a complete movement-conflict matrix or national road law.
- Lane changes require an adjacent, compatible lane and clear front/rear gaps;
  there is no destination routing or unrestricted obstacle overtaking.
- Positions are planar. The generator/host must separate layers and avoid
  connecting a bridge to the road below it.
- The host supplies world obstacles, pedestrian crossings and signal metadata.
  Signal phases are inferred unless a host supplies surveyed timing.
- Cached positions and a sorted spatial sweep reduce ordinary neighbour
  searches. Nested junction checks can still produce O(n³) work in live agents.
