# Saida Wildlife

`plugins/wildlife` is an optional, header-only C++ add-on that lives the
lives of ambient animals: groups that stay around a home, stand, graze,
wander, keep together and run from whatever comes too close. It is not linked
into `saida_engine`, it is absent from CMake, and it is never loaded by the
editor, the runtime or an exported player. A project that includes the header
compiles it into its own binary, as with `plugins/traffic`.

## What it is

`include/saida/wildlife/Wildlife.hpp` uses the C++ standard library only. **It
knows nothing about the engine** — no scene, node, clip, physics or glm — and
nothing about where its groups came from: the host says which kinds exist,
where each group lives, which ground each kind may stand on and what
threatens them, and reads back where every animal is and what it is doing.

| Type | Responsibility |
| --- | --- |
| `Kind` | How one kind of animal behaves: walk, run and flee speeds as drawn, flee distance and style (`None`, `Run`, `Dart`), group spread, roam radius, standing and walking spells, air surface and hover band, view range. |
| `Group` | A group that lives somewhere: its home, the kind of each member, a stable identity the host chooses. |
| `Herds` | A population of groups over fixed slots. `update()` lives their lives; `animals()` says where each is and what it does. |

## Model

A group lives anywhere in its roam (its home range) around its home: at any
moment it is at one point of it, drawn on ground it may stand on, and drawn
again every 45 s while unseen. It is spawned whole when that point is within
its leader's view range of the observer and the host says it is out
(`setActive`: its hour, season, weather are the host's business), so it is met
as often as one passes where it is. It never spawns inside its own flight
distance, where it would bolt at once; close by it is never spawned in plain
sight, only behind the observer's back; further out it may come into view. An
animal in view is kept out to three view ranges; out of sight it is dropped
beyond 1.3 view ranges, or once the host sent its group in. A dropped or
darted group waits before it may come back.

The first member leads: it stands or eats for a spell, then walks a leg of
its roam, turning on the way and slowing in tight turns. The others keep a
loose formation behind it and run to catch up when far behind. Animals keep
apart by their radius. Everything they stand on, walk through or flee onto is
tested by the host's `Allowed` callback, so a deer never walks into a lake.

A threat inside a kind's flee distance (scaled by the threat: a car is feared
from further than a walker) scares the whole group: `Run` kinds flee away from
it, turned toward allowed ground, at their flee speed until they calm down;
`Dart` kinds — small animals — dart a short way and are gone; `None` kinds
stay. A kind without a gait never moves; a kind that cannot stand keeps
walking (an animal without a standing clip). Air kinds hover inside their band
and sit on the ground when they stand.

Spawns are seeded from the group's identity and the host's seed, so the same
groups visited again draw the same animals in the same places. Slot indices
are stable while an animal lives; `Animal::identity` grows with every spawn
into a slot, so a host can tell reuse apart.

## Use

```cpp
#include "saida/wildlife/Wildlife.hpp"

saida::wildlife::Herds herds;
herds.reset(kinds, groups, /*slots*/ 24, seed);
herds.setAllowed([&](uint16_t kind, saida::wildlife::Vec2 at) { return habitat(kind, at); });

// each frame
for (size_t g = 0; g < groups.size(); ++g) herds.setActive(g, outNow(g));
saida::wildlife::Scene scene;
scene.observer = {eyeX, eyeY};
scene.facing = {lookX, lookY};
scene.threats = {{{playerX, playerY}, 1.f}};
herds.update(dt, scene);
for (const auto& a : herds.animals()) {
    if (!a.alive) continue;          // a dead slot keeps its place
    draw(a.position, a.heading, a.state, a.speed, a.height);
}
```

## Verification

One translation unit, no engine build:
`g++ -std=c++17 -O2 -Wall -I../include test_wildlife.cpp -o test_wildlife` in
`plugins/wildlife/tests`, which prints `PASS saida::wildlife`.
