# Sunset Strip — SaidaEngine open-world slice

A third-person open-world city: drive, shoot, take missions, and ride a jet ski
off the beach. Built to exercise the engine's physics, traffic, weapons and
scenario systems the way the vertical slice exercises its renderer.

## Run it

```sh
./build/bin/SaidaEngine.exe --project GTAClone --play
```

**Controls** — `ZQSD`/`WASD` move, mouse looks, `Space` jumps, `Shift` sprints.
`F` gets in and out of any car; at the wheel the same keys drive, `Space` is the
handbrake, and holding a direction while upside down rocks the car back onto its
wheels.

## Layout

| Path | What it is |
| --- | --- |
| `roadnet.py` | The road graph. **The layout's source of truth** — the tiles, the blocks and the lane graph all derive from it. |
| `gen_city.py` | Generates the city, the beach and the interiors from that graph. **The scenes' source of truth** — edit this, not the generated scenes. |
| `scenes/city.scene` | Generated world. |
| `scenes/city.roadnet` | Generated lane graph, for the traffic system. |
| `scripts/` | Gameplay glue and the headless E2E harnesses (QuickJS). |
| `assets/` | The CC0 kits, the shared character rig, audio and the sky. |

## The city

400 m square on a 50 x 50 tile grid: seven avenues and seven streets, plus a
seafront boulevard, giving 568 road tiles across 42 blocks and 56 junctions.
Three districts — a downtown of towers around the core, residential streets
around it, and a port strip along the water — then sand sloping into a realistic
sea with a dock.

Which kit tile goes on a cell, and how far it is turned, is **searched for**
rather than hard-coded: each tile is described by the set of edges a road
continues through (read out of the meshes by rasterising their kerbs), and the
generator looks for the piece and yaw whose openings match. A wrong reading
fails loudly instead of quietly producing roads that do not join up. Buildings
are placed the same way — the generator measures each model and finds the yaw
that turns its facade to face the street.

Four buildings are enterable. Their interiors are sealed rooms 60 m below the
city, reached by a teleport, because the building models are solid and an
interior cannot be carved out of one. Both ends of each doorway are wired so
neither can land the player inside the other's volume, and the street-side
volume sits on the pavement between the kerb and the facades — placed any
deeper, the character controller pushes the player out of it before the trigger
can fire.

Colliders are explicit boxes, never the models: a body builds its shape from the
FIRST drawable mesh in its subtree, so a body holding a multi-mesh kit model
would collide with one piece of it and let the player walk through the rest.

### Ground levels

Every horizontal surface has its own height, and the constants at the top of
`gen_city.py` are the only place they are decided. Two coplanar faces z-fight,
and the bare ground, a block's paving and every building's base all sitting at
zero made the city read as though it were sinking into itself.

A road tile is 0.02 units thick, which is **0.16 m** once scaled by `TILE`, and
its asphalt is the middle of its three levels rather than its top. Laying the
tile at ground level therefore raised the road 10 cm above the plane the player
walks on. `ROAD_Y` is the tile origin that lands the asphalt on `ASPHALT_Y`,
leaving the kerb standing `KERB_H` proud of it; a block's paving is a body, not
just a slab, so the player steps up onto it instead of walking with their feet
inside it.

### Cars

The kit's car models **already contain their wheels**, as `body` and
`wheel-front-left` and so on inside the one mesh. The separate `wheel-*.obj`
models are spares: dropping four of them onto a car puts a second, larger set of
wheels over the first and makes the chassis look far too small. A driven vehicle
needs the wheels split out of the body so the suspension can move them, which is
`tools/split_car_wheels.py`: it writes `<name>-body.obj` and one recentred
`<name>-wheel.obj` per car, and records every car's wheel anchors and radius —
measured off the art, never typed in — into `assets/models/cars/vehicles.json`.
`drivable_car` in the generator reads that manifest, so a car's suspension cannot
quietly disagree with the model hanging off it.

**Every car in the street drives.** All 30 are a `RigidBody` carrying the
`Vehicle` behaviour, a chassis box **lifted clear of the road** — the wheels are
rays and carry the car, so a chassis that reached the ground would rest on the
asphalt and leave the suspension nothing to do — and four wheel meshes named
`WheelFL`, `WheelFR`, `WheelRL`, `WheelRR` that the behaviour places, steers and
spins. A city where one car in thirty opens is worse than one where none do,
because nothing tells the player which.

They are parked on the asphalt rather than the pavement: a car started astride a
kerb settles into a lean before anyone touches it. Nothing parks in the player's
own lane for eight tiles either side of their car — cars park at `X(i) +/- 2.4`,
which is exactly that lane, so without it the first car anyone opens is boxed in
nose to tail. The single wreck inside one of
the interiors stays a plain mesh with no body — it is a prop in a room, not a
car.

Their mass is placed on the axle line rather than at the centre of the collision
box (`centerOfMass`, measured off the art like everything else). Jolt takes the
centre of mass from the geometry, and a box that has to cover the roof puts it
far too high: left there a car tips at less than its own tyres can pull and rolls
over in any hard corner.

Cost: 30 dynamic vehicles measured **below the run-to-run variance** of the frame
time here (6.8 ms and 8.7 ms uncapped for the full fleet and for one car, which
is the noise floor, not a saving). What will constrain traffic is triangles and bodies, not
the vehicle solver.

**Getting in.** Walk up to any of them and press `F`. Press it again to get out;
that is refused above 2 m/s, and lands the player beside the car rather than
inside the shell it collides with.

`scripts/driver.js` owns the handover, and it runs on **its own node** —
`DriverControl` — for two reasons worth stating because both were learned the
hard way:

- A character and a vehicle read the **same** movement actions, so exactly one
  thing may be listening. Every car is authored with `readsInput` off and is
  only ever moved from that script; one driver holding at most one car makes the
  conflict impossible by construction, where a script per car would put all 30
  of them in a race to answer the same key press.
- It cannot ride on the player, because seating someone disables the player's
  node and a disabled node stops running its behaviours. A driver that lived
  there would switch itself off the instant it got in, and stay in the car for
  ever.

Which car opens is decided by `physics.overlapSphere` against the real
colliders, not by distance to an origin: a van is four metres long, and measuring
to its centre would open it from inside its own bonnet while refusing the same
player at its back doors. When several overlap, the nearest wins.

The camera follows the `camera_target` group rather than a node, and getting in
moves that membership from the player to the car — that is what carries the view
across, with no camera code at all.

Regenerate the world with:

```sh
python GTAClone/gen_city.py
```

Placement is seeded, so the same script always produces the same city.

## Scale

The three city kits (commercial, suburban, roads) share one internal scale in
which a road tile is exactly 1 unit. `TILE = 8.0` metres converts it: a road tile
becomes a two-lane carriageway of two 4 m lanes, and the kits land on believable
sizes — a commercial building 7.0 x 7.5 m and 10.3 m tall, a skyscraper 10 x 11 m
and up to 32.6 m, a suburban house 10.4 x 8.2 m and 6.6 m, a roundabout 24 m
across.

The car kit does not share that scale and is converted separately. It is authored
chunkier than life (a sedan is 1.50 x 1.30 x 2.55), so `CAR = 1.25` puts it at
1.9 x 1.6 x 3.2 m against the character kit's 1.75 m person.

## Verification

```sh
SaidaEngine.exe --project GTAClone --play --test-autoload "E2E=scripts/e2e_smoke.js"
```

```sh
SaidaEngine.exe --project GTAClone --play --test-autoload "E2E=scripts/e2e_drive.js"
```

```sh
SaidaEngine.exe --project GTAClone --play --test-autoload "E2E=scripts/e2e_enter_car.js"
```

```sh
SaidaEngine.exe --project GTAClone --play --test-autoload "E2E=scripts/e2e_car_reach.js"
```

All four print `PASS` and must leave zero `[error]`/`[warn]` lines.

`e2e_drive.js` covers the car itself: that it settles on its springs rather than
its chassis, that its four wheel meshes hang where the suspension says, that the
throttle builds speed and moves it along its own heading, that steering turns it
and the handbrake stops it.

`e2e_enter_car.js` covers the handover, which is the part that can go wrong
without anything looking broken: that a **parked car ignores the walk keys**,
that getting in moves the camera, that the player does not walk while seated,
that getting out puts them beside the car and leaves it parked, and that they
walk again afterwards under the very same action.

`e2e_car_reach.js` covers which car opens and from where: that one opens from
2.6 m beyond its own origin — past the reach radius, so this can only pass if
reach is measured against the collider — that standing at two different cars in
turn opens each of them rather than a fixed favourite, and that pressing the key
in the open street does nothing.

All of them press the keys a player would through `input.inject`, and all get in
the way a player does. None disables the player to reach a car: proving the
physics against a rig nobody can reach would prove the wrong thing. The harness asserts
what the scene declares, that the player settles on the ground rather than
falling through it, and that a street door actually moves them into its
interior. A model that failed to load leaves its node in place, so the absence
of errors in the log is what proves the meshes actually read.

The world is **1.34 M triangles** scene-wide. Treat that as a ceiling: the frame
rate is comfortable there, so anything added from here — traffic, pedestrians,
mission props — has to come out of that budget rather than on top of it.

Should it need cutting, the lever is an LOD chain per kit model. The project's
`autoMeshLods` flag will not do it: it only applies to models brought in through
the editor's importer, not to meshes a generated scene references by path.

## Assets

Everything is **CC0** (public domain, no attribution required), and nothing here
was authored by hand. Every file is declared in `compliance/assets.json`.

| Source | Used for |
| --- | --- |
| [kenney.nl](https://kenney.nl) — City Kit (Commercial), City Kit (Suburban), City Kit (Roads) | buildings, houses, roads, sidewalks, street furniture |
| [kenney.nl](https://kenney.nl) — Car Kit | the traffic fleet and its separate wheels |
| [kenney.nl](https://kenney.nl) — Pirate Kit | the rowing boat hull the jet ski is built on |
| [kenney.nl](https://kenney.nl) — Animated Characters, Blaster Kit, audio packs | the rig and its clips, weapons, every sound |
| [polyhaven.com](https://polyhaven.com) — *Kloofendal 48d Partly Cloudy (Pure Sky)* | the 4K HDRI sky and its IBL |

### Pipeline

None. Unlike the nature kit used by the vertical slice, these five kits ship a
valid OBJ variant with one shared `colormap.png` per kit, so they need neither
the scene-root repair nor the sRGB linearisation, and they instance cheaply as
`MeshNode` + atlas rather than being imported per instance.
