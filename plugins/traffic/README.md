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

One header, `include/saida/traffic/Traffic.hpp`, with three types:

| Type | Responsibility |
| --- | --- |
| `Graph` | Nodes and directed lanes in one coordinate frame, plus the adjacency built once from them. Immutable while driven on. |
| `Rules` | How this place drives: side of the road, lane offset, acceleration, braking, headway, junction guard, spawn and despawn radii. |
| `Flow` | A population of agents on one graph. `update()` drives them; `pose()` says where one is. |

It depends on `<cmath>`, `<cstdint>` and `<vector>`. **It knows nothing about
the engine** — no scene, no node, no material, no serialization, no glm, no
coordinate system beyond "two axes the ground lies in". The host owns
rendering, owns where the graph came from, and reads poses back.

That seam is what makes it testable without a window and reusable by a
hand-built city as readily as by a streamed planet.

## Model

Agents on a directed lane graph, a car-following rule, a give-way rule at
junctions, and spawning in an annulus around the observer so traffic exists
where it is seen and nowhere else — the model the sixth-generation open-world
games shipped.

There is no path planning, no destination, no traffic signal and no lane
change. Each is a feature that can be built on top of this; none is needed for
a street to stop looking abandoned, and every one costs frame time on every
car.

Cost per update is O(n²) in live agents, deliberately: at the tens of cars a
street can show, a spatial index costs more to maintain than the comparisons it
saves. A host that wants thousands wants a different class.

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

`agents()` is a fixed-size array whose indices are stable while an agent lives,
so a host keys its own per-car state (a scene node, a colour) by index and
releases it when `alive` goes false. Lowering the population truncates the
array; a host must drop the state of any index past the new size.

A host can pool its scene nodes per slot to avoid repeated allocation and model
loading. In Saida, use `Node::setVisible(false)` between trips and restore
visibility on reuse. Hidden nodes retain their resources; visibility changes
refresh only the affected scene branches. Keep the pool scoped to its region.

`facing` is the direction the observer is looking, or `{0, 0}` when the host
does not track one. Given it, a car may appear much closer behind the observer
than in front: the hole in front is what a player sees, and the one behind is
what makes a city feel empty for no reason.

A streaming host gives each streamed region its own `Graph` and its own `Flow`.
Discarding the region then discards its traffic without telling any other agent
about it, and a car that reaches the edge of its region's graph retires there.

## Verification

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

- Junction give-way is nearest-first, not a priority or signal system.
- No lane changing, so a lane is a queue behind its slowest car.
- No vertical dimension: the graph is planar and a host that needs bridges
  gives them their own graph.
- An agent knows the lane it is on and nothing about the world beside it. A
  host that wants cars to stop at a level crossing puts an `Obstacle` there.
- `Flow::update` is O(n²) in live agents, but each car's position is computed
  once per step rather than once per comparison: the square roots are O(n).
