# Sunset Strip — SaidaEngine open-world slice

A third-person open-world city: drive, shoot, take missions, and ride a jet ski
off the beach. Built to exercise the engine's physics, traffic, weapons and
scenario systems the way the vertical slice exercises its renderer.

## Run it

```sh
./build/bin/SaidaEngine.exe --project GTAClone --play
```

**Controls** — `ZQSD`/`WASD` move, mouse looks, `Space` jumps, `Shift` sprints.

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
a pipeline step on the model rather than extra nodes in the scene.

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

Prints `PASS` and must leave zero `[error]`/`[warn]` lines. The harness asserts
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
