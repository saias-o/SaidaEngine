# SaidaEngine - Canonical specification

Updated: 2026-07-24. This document is the engine's technical truth. It describes
what actually exists, the V1 candidate contracts and the limits. Work to be done
lives only in [ROADMAP.md](ROADMAP.md).

All documentation and all code comments must be written in English. A comment or
document in any other language is a defect to fix.

Automated agents must never add themselves, their vendor or their tooling as
commit co-authors. Agent-assisted commits must not contain `Co-authored-by`
trailers for an AI agent; authorship remains with the human or service account
that owns the change.

## 1. Product and invariants

SaidaEngine makes it possible to create desktop, Web and XR 3D games with a
desktop editor, a standalone runtime and an authoring surface drivable by Saida.

Invariants:

1. The scene model, behaviours and gameplay APIs are shared across platforms. A
   backend may be missing, but must not invent a second engine.
2. Vulkan and WebGPU use the same `Renderer` through the RHI; OpenXR replaces
   presentation without duplicating the render pipeline.
3. With identical content, camera and settings, the visual result must be
   identical on every platform. Lighting, materials, shadows, colorimetry and
   post-processing cannot have platform-specific behavior or rendering; any
   divergence is a regression to fix, not an accepted variant.
4. The engine keeps, as much as possible, a single code and a single execution
   path shared across all platforms. A platform-specific implementation is
   admitted only when a platform constraint truly requires it, notably at the
   RHI, presentation or system-API level; it must not duplicate engine logic or
   modify the rendering.
5. QuickJS is the sole JavaScript runtime. RmlUi is the sole HTML/CSS UI system;
   Ultralight/JavaScriptCore are no longer part of the product.
6. Durable mutations go through validated SaidaOps then a snapshot.
7. A missing capability is visible via `PlatformCaps` and must fail or degrade
   explicitly.
8. Project, script and import paths stay under the project's canonical root.
9. Every public piece of data is versioned and migrated. An unknown type or a
   future schema is rejected, never silently transformed.
10. The engine is designed primarily for Web and mobile targets. Architecture
   and implementation choices must therefore minimize startup time, CPU/GPU
   cost, allocations and memory footprint.
11. On critical paths, favor `O(1)` complexity whenever reasonably possible.
   Failing that, keep the most efficient algorithm and data structures, with
   particular attention to startup and to code executed every frame.

The engine favors RAII, explicit ownership, node composition and small
abstractions. No heavy ECS, general-purpose render graph or genre framework
without a measured need.

Gameplay contract, human or AI:

1. All logic is a small `Behaviour`; a node composes data and behaviours.
2. Compose several focused responsibilities, do not create a god-controller.
3. Call down, signal up: direct calls to descendants, typed signals to the
   parent or at a distance.
4. No gameplay singleton. Only engine services and autoloads are global;
   persistent state lives in an autoload.
5. Find by group or scoped query, not by global name.

`listen()` carries the lifetime of connections. Tree mutations use deferred
`changeScene`/`queueFree`. A cross-cutting event bus is a typed signal carried
by an autoload, not a new global.

## 2. Execution architecture

### 2.1 Processes and libraries

- `saida_engine`: static library containing runtime, rendering, scene, physics,
  scripting, UI and assets.
- `saida_editor`: ImGui tools and Hub, never linked into the exported player.
- `SaidaEngine`: desktop editor. Without `--project`, it opens an empty scene.
- `SaidaEngineHub`: project creation, selection and opening.
- `SaidaEngineRuntime`: standalone desktop game template. It reads `game.saida`,
  mounts the packaged project and launches the main scene.
- `saida_tool`: headless validation, fold, inspection and export.
- `saida_player`: WebAssembly/WebGPU game player.
- `saida_authoring`: headless authoring-core WASM loaded by the platform.
- `saida_web`: visual WASM/WebGPU runtime embedded in Saida Online.

### 2.2 Ownership and loop

`Engine` owns the subsystems and orchestrates a frame: input, time, deferred
updates, physics, scene, audio, UI, rendering and presentation. Vulkan resources
are RAII, non-copyable and borrow the device. The declaration order must
guarantee that the device outlives its resources.

VMA v3.1.0 is vendored under `third_party/vma`; the public headers use forward
declarations and a single TU defines `VMA_IMPLEMENTATION`. Same rule for
stb/tinyobj. Descriptor sets are organized by frequency: set 0 global per frame,
set 1 per material, model matrix in a push constant. Application code does not
manipulate VMA types directly.

`SceneTree` carries the active scene, the persistent `World`, the autoloads, the
timers and the deferred scene changes. A destructive mutation cannot invalidate
the current frame's render caches; `refreshHierarchy()` is called after the
deferred operations.

Paths go through `core/Paths`. In editing, the root comes from the loaded
project. In a packaged runtime, it comes from the executable's folder. Relative
paths are normalized and symlinks that leave the root are rejected.

### 2.3 Capabilities per runtime

| Capability | Desktop | Web Player | Web/headless Authoring |
|---|---|---|---|
| Rendering | Vulkan | WebGPU | WebGPU for the viewer, no GPU for the fold |
| Physics | Jolt | single-thread Jolt | outside viewer/fold |
| Audio | miniaudio | Web Audio | not required |
| JavaScript | QuickJS | QuickJS | targeted validation |
| Game UI | RmlUi | RmlUi HUD `UICanvasNode`/`UITextNode`; WebCanvas absent | partial authoring |
| Keyboard/mouse | yes | yes | browser/host UI |
| Gamepad | GLFW standard | Gamepad API, `standard` mapping | not required |
| Pad haptics | no (GLFW) | `dual-rumble` if exposed by the pad | not required |
| Touch | no | raw + zones/tap/swipes | host |
| Player storage | files | IDBFS | not required |
| XR | OpenXR | no | no |

The `PlatformCaps` report is logged at boot. Desktop announces everything except
touch. The Web player announces only the backends actually linked.

## 3. Project, scene and authoring

### 3.1 Project

A `.saidaproj` describes, among other things, the name, the main scene, the
autoloads, the audio aliases and the project files. The AssetRegistry associates
a stable AssetID with a relative path. Since schema 2 an entry may carry an
`import` block (`wrap`, `srgb`) holding the project's import overrides. Every
field is optional and absence is meaningful: it keeps whatever the source asset
declares. A value is an explicit override and wins over the asset, so a
third-party glTF exported with the wrong wrap mode is corrected in the project
rather than re-exported — and without rebuilding the engine. The block is
written only when it holds something, an unreadable one is reported and dropped
rather than guessed at, and a value silently ignored would be the worst outcome
of all: it would send an author hunting through their exporter for a setting the
project had already fixed. Local caches are not a source of truth.
The engine's public identity is defined once in `core/EngineVersion.hpp`
(`1.0.0`). Every V1 project records this version. Documents without the current
envelope or produced by an incompatible pre-release are rejected: no migration
or backward-compatibility branch is promised before stable publication.

Autoloads can be scenes, native behaviours or `.js`/`.mjs` scripts. The `World`
persists across scene changes. Nodes support hierarchy, groups, signals,
transforms, activation and deferred removal.

A scene's rendering settings (the `settings` block: ambient, clear colour, GI,
skybox, IBL, AO, fog, bloom) have exactly one reader and one writer,
`applySceneSettings`/`writeSceneSettings` in
`src/scene/SceneSettingsSerialization.hpp`. Both entry points go through them —
`Scene::serialize`/`deserialize` for the block embedded in a scene node, and
`SceneSerializer::loadIntoScene` for a scene file. The writer emits every field,
so an engine-written document is always complete. The reader *patches*: a field
the document omits keeps the value already in place, which is what a scene
instance overriding a single setting needs; a scene file therefore resets to
`SceneSettings{}` first, so what it omits is the engine default and never the
previous scene's. An asset reference inside that block is durable as a
project-relative **path** (`"skyboxTexture": "assets/skies/sky.hdr"`), resolved
through the registry at load; the numeric `AssetID` form is still read, but ids
are regenerated whenever the registry is re-keyed (§13), so a pasted id is not a
durable reference. An explicitly empty path clears the reference rather than
keeping the previous one. `saida_scene_settings_tests` locks the round trip, the
patch semantics and both reference forms.

### 3.2 SaidaOps

The authoring contract covers the validated operations of creation/deletion,
transform, property, reparenting and other mutations declared by
`EngineManifest`. The same code serves desktop, `saida_tool`, the WASM gateway
and the authoring runtime when the type is registered.

Rules:

- the UI does not fabricate arbitrary JSON outside the contract;
- the manifest is the authority on types, properties, enums and bounds;
- an invalid operation is not persisted;
- a durable batch is atomic by default;
- `--skip-invalid` is reserved for diagnostics and does not publish a durable
  snapshot;
- `opVersion: 2` addresses all nodes by stable `NodeId` encoded as a 64-bit
  decimal string, so as to lose no bit in JavaScript; `parentId`, `newParentId`,
  `fromNodeId` and `toNodeId` carry the other references. An omitted parent
  designates the root. Names are never accepted as durable references.
  `create_node` accepts a `nodeId` provided by the client so that a
  deterministic batch can immediately target the node; otherwise the applier
  generates the ID and returns it in its diff.

### 3.3 Snapshots and registries

Every durable format writes equal `schema` and `version` and delegates their
check to the single guard `format::schemaEnvelopeError`: `schema`/`version` must
be integers, must agree — a divergence signals a falsified or non-conforming
document — and must not exceed the supported version. Snapshot, scene, project,
asset registry and scenario share this fail-closed rejection with a diagnostic
naming the format; earlier versions are migrated in memory. The headless fold
round-trips its registered subset, including `Node`, existing `MeshNode`,
`Camera`, `Area` and `ScriptBehaviour`, as well as the
`UINode`/`UICanvasNode`/`UITextNode` HUD. The `CollisionShape`, `StaticBody`,
`RigidBody` and `CharacterBody` types also keep all their durable properties
without starting Jolt. Mesh references stay opaque without a GPU.

Creating a `MeshNode` during a fold without a `ResourceManager` is rejected. The
advanced UI types stay out of the headless registry as long as they are not part
of the V1 player contract. The player and Web authoring reject absent
types/behaviours before moving to `ready`.

`RuntimeTypeMatrix.hpp` is the single V1 inventory of the four runtimes'
factories. It distinguishes `required`, `optional` (conditional native XR) and
`absent`, is published in `EngineManifest`, then compared to the effective
registries at boot/snapshot. Any missing or undeclared required factory fails
the runtime with a diagnostic. The registries are not yet equivalent: the matrix
makes each divergence explicit, it does not grant permission to lose content.

The `saida_runtime_type_matrix_tests` test builds the headless corpus from the
matrix: 17 node types (physics joints included), 16 behaviours and the 138
reflected properties receive non-trivial values, then a serialize/load/serialize
cycle must stay semantically identical. It also covers the hand-written data of
the HUD, the bodies/colliders, the `Blackboard`, the FSM and `ScriptBehaviour`.
Web authoring runs its contractual snapshot before publishing `ready`.

`RuntimeRoundTripContract` builds an in-memory corpus the same way. The full
serializer covers native (29 nodes, 20 behaviours, 138 properties in the current
XR build) via `SaidaEngine --verify-runtime-contract`, and the Web player
(18/10/130) via the `verify-runtime-contract` parameter. The snapshot codec, now
without `ResourceManager`, covers Web authoring (9/0/90) before its transition to
`ready` and the headless one (17/16/138). All require a semantic JSON identity
after reconstruction and expose the `[CONTRACT] PASS` verdict.

`saida_tool verify-manifest` closes the loop from the binary actually shipped: it
generates the manifest, requires each announced node/behaviour to be a line of
the round-tripped `runtimeTypeMatrix`, requires the live headless registry to
match that matrix, and runs the headless snapshot round-trip. No announced type
can therefore escape the round-trip proof. The command is run in CI on the
produced `saida_tool` artifact.

## 4. Rendering and GPU resources

### 4.1 Renderer

The renderer supports Vulkan 1.3, Dynamic Rendering, VMA, depth, desktop MSAA,
PBR metallic-roughness, lit/unlit materials, IBL, ACES, directional, point and
spot lights, PCF shadow mapping, SSAO, bloom, fog, xatlas lightmap baking and
DDGI. The depth pass skins its casters from the same bone palette as the scene
pass, so an animated character casts the shadow of the pose it is actually in;
the shadow pipeline therefore binds set 0, where that palette lives. Point lights
do not yet have a shadow cubemap. Lightmaps are regenerated
and are not yet part of the durable package.

The RHI compiles to Vulkan or WebGPU. GLSL shaders are compiled to SPIR-V, then
transpiled to WGSL with naga for the Web. The Web player has no MSAA. XR
rendering uses the same renderer with stereo/multiview views.

The GPU-driven path has bindless materials, indirect draw, compute culling and
tested binding contracts. It is not the active universal path: some
`useGpuDriven=false` remain. Its activation must become an explicit setting/cap
and be benchmarked against the classic path on heavy scenes. Performance claims
require reproducible scenes, published GPU/driver, resolution, number of
lights/draws/particles and CPU/GPU frame times.

The desktop editor profiler exposes indexed mesh geometry as
`Renderer/FrustumTriangles` and `Scene/TotalTriangles`, displayed together as
camera/scene triangles. The scene value sums every active renderable mesh
instance at its renderer-selected LOD; the camera value sums the subset whose
bounding spheres intersect the camera frustum. They describe per-instance
indexed mesh workload, not exact pixel visibility: occluded and back-facing
triangles still count, while shadow re-submission, particles, water, UI and
post-processing do not. Collection runs only while the desktop Profiler is open
and enabled. Its collector belongs exclusively to `saida_editor`; standalone
desktop and Web players neither link it nor traverse their scenes for these
diagnostics.

### 4.2 AssetRegistry and AssetLoader

`AssetRegistry` is the only base of identities. `AssetLoader` exposes
asynchronous handles with states `queued`, `loading`, `ready`, `failed`,
priorities `low`, `normal`, `high`, `critical`, error, size, id and `release()`.

Textures and `.obj` meshes follow this path: worker read/decode on desktop,
`pump()` on Web, then GPU creation on the main thread. During loading, a fallback
is visible; a failure uses a magenta checkerboard. Mesh proxies stay stable and
physics rebuilds the body when the mesh becomes available.

The standalone animation files `.srig`, `.sclip` and `.sgraph` follow the same
contract: JSON read and parse on the desktop worker or in Web `pump()`, typed
payload finalized by `ResourceManager` on the main thread, state and diagnostic
consultable without waiting. `CharacterBehaviour` requests its graph then applies
it only at `ready`; the Animation panel keeps the Play/Edit/Apply/Inspect action
and finishes it on a later frame. An invalid document moves to `failed` without
replacing the live graph or view.

The memory registrations `registerMemoryRig`/`registerMemoryAnimation` AND
`registerMemoryMesh` (keyed flavor `model.gltf#meshN_primM`) are idempotent:
re-importing the same glTF keeps the existing instances and returns the same
AssetIDs — the attached Animators/MeshNodes keep valid pointers and a snapshot
restored after eviction references resolvable ids.

On `changeScene`, `trimUnused` performs a mark-and-sweep: textures, meshes,
materials, plus the rigs/clips that no living Animator holds anymore and the
ClipView/AnimGraph caches (pure file caches, reloaded by path). GPU destruction
is deferred four frames; bindless indices and material slots are recycled.

DURING a scene, `gpuBudgetBytes` (512 MiB by default, `assets.setGpuBudget`) is
applied every frame: beyond it, textures/meshes neither referenced by the living
scene (usage snapshot refreshed on every hierarchy change) nor being loaded are
evicted LRU (`lastUse` per frame), counters `gpuEvictedCount/Bytes` in
`assets.stats()`. If the entire overshoot is referenced: a single warning,
nothing broken. `gpuResidentBytes` is exposed to `assets.stats()` and to the
profiler; the desktop and Web E2E verifies stability over 16 cycles, real
mid-scene LRU eviction and a hitch threshold (max dt published in the verdict, CI
ceiling 2 s).

Hostile content: `cgltf_validate` after each parse (out-of-bounds accessor
rejected before any read — fatal in wasm), OBJ with no usable geometry rejected
at decoding (`failedTotal` cumulative in `assets.stats()`), empty geometry
rejected at GPU creation. WitnessGame embeds a deliberate `corrupt.obj` and a
`corrupt.glb` traversed by the three harnesses.

On Web, `project-files.json` separates the MEMFS boot from the streamed assets:
PNG/JPG, OBJ and `.srig/.sclip/.sgraph` are served on demand by fetch via the
AssetLoader. Scenes, scripts, glTF/GLB, `.sseq` and `.sretarget` stay preloaded
as long as their consumers are not asynchronous.

### 4.3 Media formats

- Audio: `.ogg` Vorbis recommended; `.wav` accepted. MP3 and FLAC not compiled.
- Meshes: glTF/GLB and OBJ. Meshopt is decoded at import and exported from the
  import UI ("Export meshopt GLB", quantized + EXT_meshopt_compression).
- Textures: PNG/JPG (stbi) on all platforms — V1 decision: no KTX2/Basis (no
  heavy textured content that justifies the transcoder; P2).
- Texture wrapping follows the glTF sampler: `CLAMP_TO_EDGE` on either axis
  clamps the texture, anything else repeats it, on Vulkan and WebGPU alike. It
  matters for cut-out sprites, whose UVs often overshoot `[0, 1]`: repeating
  tiles a second copy of the art beside the first, while a tiling surface
  (walls, ground) clamped by mistake smears one edge texel across itself.
  Neither is a rule the engine can guess — it is per asset, so the asset
  declares it and `asset_registry.json` can override it (3.1).
- Tangents: without author tangents in a glTF, the material's normal mapping is
  explicitly disabled (warning logged) — never lighting silently approximated;
  MikkTSpace is P1.

### 4.4 AutoLOD

AutoLOD is a separate tool based on meshoptimizer/xatlas. It generates GLB LODs,
accepts ratios and error thresholds, preserves open edges for modular assets, can
produce standalone LODs, bake a normal map from the high-poly and create a
far-distance proxy with a new unwrap/atlas.

```sh
./build/autolod.exe rock.glb
./build/autolod.exe building.glb out.glb --ratios 0.7,0.4,0.2,0.05 --errors 0.005,0.02,0.05,0.1
./build/autolod.exe wall.glb out.glb --lock-border
./build/autolod.exe helmet.glb out.glb --bake --bake-res 1024
./build/autolod.exe helmet.glb out.glb --split --bake --bake-res 2048
./build/autolod.exe helmet.glb out.glb --proxy --ratios 0.6,0.3,0.1,0.03
./build/autolod.exe --gen-test sphere.glb
./build/autolod.exe --dump-images out.glb prefix_
```

Options: `--ratios`, `--errors`, `--lock-border`, `--uv-weight(s)`,
`--normal-weight`, `--bake`, `--bake-res`, `--bake-cage`, `--split`, `--proxy`.
Without bake, high UV weights can prevent reaching the requested ratio; with
bake, the weight drops back to 1 and the normal map restores the detail. Below
0.15, `--proxy` welds by position, re-decimates, recomputes normals, redoes an
xatlas atlas and bakes albedo, normal, metallic-roughness, occlusion and emissive
into a standalone material. It implies `--bake`.

The bake raycasts LOD0 via BVH/Möller-Trumbore, recomposes the source normal map
into the LOD's tangent basis and dilates the islands. The tool preserves
materials, textures and hierarchy, welds the vertices, uses QEM with attributes
then the sloppy fallback, optimizes the vertex cache and writes `MSFT_lod` with
`MSFT_screencoverage`. LOD0 stays intact.

Limits: skinned meshes ignored, 32-bit indices, single-thread bake, source UVs
expected as `[0,1]` islands outside proxy, overhead of one welded vertex buffer
per LOD. AO/curvature, multi-material atlas, impostors and virtualized clusters
remain extensions. Aggressive settings must be validated visually.

## 5. Gameplay

### 5.1 Physics

Jolt provides rigid bodies, collision shapes, character body, areas/triggers,
layers and scene integration. The Web player uses a single-thread job system. The
character has an inner body to be visible in broadphase, sensors and raycasts.

**Collision shapes.** `Auto`, `Box`, `Sphere`, `Capsule`, `ConvexHull` (dynamic
capable) and `Mesh` (exact triangle mesh, static bodies only) are all
implemented; the hull and triangle-mesh builders fall back to a box, with a
logged warning, when the body carries no CPU mesh data. Current limit: a shape
resolves against the **first** mesh found under its body, so a body holding
several meshes — an imported level in particular — collides with one of them and
ignores the others without any diagnostic. Levels must therefore be split into
one body per mesh until the builder covers a whole subtree ([ROADMAP](ROADMAP.md)).

**Scene queries.** `PhysicsWorld::raycast` and `overlapSphere` take a
`QueryFilter`: sensors (Area) are excluded by default — a camera occlusion ray or
a hitscan does not stop on an invisible trigger — and re-admitted via
`hitSensors`; an explicit body can be ignored (the caster). Exposed in JS by the
`physics` global (5.4) with the same semantics on desktop and Web player.

**Impulses, forces and contact quantities.** `PhysicsWorld` exposes
`linearVelocity`/`angularVelocity`, `applyImpulse` (at the centre of mass or at a
world point), `applyAngularImpulse`, `applyForce` (same two forms) and
`applyTorque`. An invalid id and a non-dynamic body absorb all of them without
moving. The distinction is a contract, not a preference: an **impulse** is
consumed whole when it is applied, whereas a **force** is accumulated and then
cleared by the next step, so a force pushed on a frame that does not advance the
fixed-timestep accumulator is silently lost. Per-frame gameplay code uses
impulses.

Two contact quantities come with them. `pointVelocity(id, p)` is the velocity of
the material point currently at `p` — linear velocity plus the body's spin — which
is what a wheel or hull patch actually moves at, and therefore what slip and drag
must be measured against. `effectiveMassAt(id, p, d)` is the mass a contact
resists with along `d`: at most the body's mass and usually well under it,
because an impulse applied off-centre also spins the body instead of only
shifting it. **Anything cancelling a velocity at a contact must divide by it.**
Using the body mass over-corrects, the correction changes sign every step, and
whatever clamp sits around it then rectifies the oscillation into a steady drift
— a failure that looks like a tuning problem and is not one. Covered by
`saida_physics_impulse_tests`.

**Joints (V1: `FixedJoint`, `PointJoint`, `HingeJoint`).** Reflected nodes
linking two bodies: `bodyA`/`bodyB` are *node paths* resolved from the joint
(`Node::findByPath`: `..`, names, `/` = root) — stable across multiple spawns
where the ids regenerated by `instantiate` would not be. Empty `bodyA` = closest
ancestor body (natural authoring form: joint child of body A); empty `bodyB` =
anchor to the world (`Body::sFixedToWorld`). The pivot (and the hinge axis, with
optional angular limits) comes from the joint node's world transform. The Scene
synchronizes the joints after the bodies: the Jolt constraint is created when
both bodies are alive and rebuilds when a referenced body has been recreated
(`markDirty`). `PhysicsWorld` owns the constraint registry: removing a body first
purges its constraints (no dangling `Body*`) and wakes the other body; removing a
constraint wakes both its bodies. Matrix: `{R, R, A, R}` like the other physics
nodes.

Advanced constraints (slider, cone, motors, breakables) and the rest of the
C++/JS parity (animation/graph/sequence/blackboard) are not yet closed.

**Character controller.** The `Character` behaviour on a `CharacterBody` is the
engine's tunable movement solver, and it is meant to be reached three ways
rather than replaced:

- **Parameters.** Roughly forty reflected properties, grouped for the Inspector
  (`Movement`, `Air`, `Jump`, `Turning`, `Ground`, `Animation`): ground and air
  acceleration and braking, air control, a gravity that can differ on the way
  down and near the apex, terminal velocity, a jump expressed as a height or an
  impulse, release cut-off, coyote time, input buffering, multi-jump, a growing
  jump chain, three turn modes with a turn-around brake, and slope sliding.
- **Functions.** `setMoveInput`, `requestJump`, `releaseJump`, `launch`,
  `addImpulse`, `faceDirection`, `setSolverEnabled` and the state accessors.
  With `readsInput` off the controller reads no device at all and becomes a
  solver its caller feeds — the mode the headless tests and a script driver use.
- **Inheritance.** `readMoveInput`/`readJumpPressed`/`readJumpHeld`/
  `readSprintHeld` decide what the character is asked to do; `onJumped`,
  `onLanded`, `onStartedFalling`, `onTurnAround` and `updateAnimation` report
  what happened. A subclass replaces an input source or reacts to a landing
  without reimplementing the solver.

The governing rule: **every parameter added to this controller defaults to the
behaviour that existed before it**. Acceleration zero means the historic
snap-to-speed, coyote and buffer times zero mean no grace, the cut-off
multiplier one means a fixed-height jump. An existing scene therefore keeps its
exact feel and opts into what it names — which is what lets WitnessGame's E2E
remain a regression test across the change. One deliberate exception, because
its old behaviour was a bug rather than a feel: horizontal speed above the
walking target now bleeds off at the braking rate instead of snapping down, so
a `launch` — the thing every special move is built from — is spent rather than
erased on the next frame. With no braking rate configured it still snaps.

**Raycast vehicle.** The `Vehicle` behaviour (`VehicleBehaviour`, matrix
`{R, R, A, R}`) attaches to a `RigidBody` whose collision box sits clear of the
road: the chassis never touches it and the wheels carry the car. Each of the four
wheels is a ray cast down from its anchor, and what the ray finds drives three
impulses applied **at the contact point** — suspension along the contact normal,
drive or brake along the wheel's heading, and a lateral impulse cancelling
sideways slip. Applying them at the contact rather than at the centre of mass is
what produces weight transfer: the nose dives under braking and the car leans in
a turn without any of it being scripted. Forward is local `+Z`. It follows the
same three-entry-point rule as the character controller: reflected properties for
layout, suspension, drive, steering, tyres and stability; `setThrottle`/
`setBrake`/`setSteer`/`setHandbrake` with `readsInput` off, which is how a traffic
system drives a street full of them through one implementation; and
`readDriveInput`/`readHandbrake` to override the input source.

Three ordering rules are load-bearing rather than incidental, because each one
was a defect that produced plausible motion and no diagnostic:

- **Normal forces resolve in one pass, before any tyre is asked what it is
  slipping at**, the way a contact solver resolves normals before friction. A
  wheel that sampled its contact after only *some* of its neighbours had pushed
  read their off-centre suspension impulses as slip of its own, and a car at rest
  crawled sideways at a steady rate.
- **Tyres resolve axle by axle, sequentially between axles and simultaneously
  within one.** Solving all four against one state removes
  `wheels x effectiveMass / mass` times too much — near 105% on a typical
  saloon — which reverses the slip and grows it every step until the car throws
  itself off the road. Letting one wheel of a symmetric pair act first is instead
  a standing left/right bias that steers a car nobody is steering.
- **The suspension damper is capped at the momentum the contact carries along
  the normal.** A damper removes momentum and must never inject any; integrated
  explicitly, `c x closingRate` outruns the momentum available once the damping
  ratio is high, and the excess comes back out as a push that launches the
  vehicle off its own springs.

**Which way is right.** The vehicle's forward is local `+Z` and its `+X` is its
**left**, matching how the kits name their wheels, so a positive steer input
turns the nose toward `-X` and the steering angle is applied as a *negative*
rotation about the vehicle's up. Both the tyre heading and the drawn wheels
follow that one convention. This is stated because a test that only asks whether
opposite inputs turn the car opposite ways passes just as happily on mirrored
controls — which is how they shipped inverted.

**Staying on its wheels.** Three things keep a car from rolling over, and only
one of them is a tuning value:

- `RigidBody::centerOfMass` moves the mass off the shape's own centroid. Jolt
  derives the centre of mass from geometry, which is right for a crate and wrong
  for a car: its mass is along the axle line while its collision box has to cover
  the roof. Left at the box centre a saloon tips at 0.59 g — under what its own
  tyres pull — so it rolls in any hard corner. Implemented by wrapping the shape
  in an offset-centre-of-mass shape, so the geometry it collides with is
  untouched.
- `rollStability`/`rollDamping` are the stabiliser the model does not otherwise
  have. Springs alone cannot hold a leaning car up: past a point the outer pair
  is at full compression and the inner pair carries nothing, so the restoring
  couple stops growing exactly when it is needed. The torque pulls toward the
  surface the car is standing on rather than world up — a banked road is not a
  fault — and acts only while a wheel is down, so a jump is left alone. Measured
  on full throttle at full lock: peak lean fell from 89 degrees, on its roof, to
  25 degrees with the car sliding and recovering onto four wheels.
- The tyre friction ceiling is held below the tipping threshold on purpose. A
  tyre allowed more lateral force than `g * halfTrack / comHeight` rolls the car
  instead of sliding it; a slide is recoverable and a rollover is not.

**Getting back up.** `selfRightTorque` lets a player rock an overturned car back
onto its wheels by holding a steering direction, the convention every open-world
driving game uses. Gated on being genuinely overturned *and* off its wheels, and
capped by `selfRightMaxSpin` so it rocks rather than spins. Without it a raycast
vehicle on its roof has nothing to push against and stays there: the only way out
of a roll is to reload. The torque must exceed `weight * halfWidth` or nothing
happens at all — it has to lift the car onto an edge before gravity can help.

A fourth rule is about time rather than order. Every force the vehicle applies
becomes an impulse by multiplying by the frame delta, so it clamps that delta to
`PhysicsWorld::kMaxSimulatedStep` — the fixed step times the substep ceiling,
which is the most simulated time one `step()` can advance however long the frame
took. Past that the vehicle would be paying force for time the world never
simulates: a 0.63 s frame while a city finished streaming turned a parked car's
suspension into 89 kN.s and threw it 28 m into the air.

Covered by `saida_vehicle_tests` with no device: resting height and wheel count,
throttle, braking to a standstill without creeping, bounded top speed, steering
direction and self-centring, a wheel over a genuine gap in the floor, an
over-damped suspension, an anti-roll bar that leans the car less rather than
more, a hard corner that slides rather than rolls, an overturned car righted by
holding a direction from four different stranded attitudes, wheel nodes placed
and spun by the suspension, and a fall back onto four wheels. Steering is
asserted by direction, not merely by motion.

Righting turns about `up x worldUp` — whatever axis takes the car back onto its
wheels soonest — rather than about its own length. Rolling is only the commonest
way to end up stranded, and a torque about the car's length cannot undo an
attitude that is not a roll. When the car is exactly inverted there is no
shortest way, and the steering direction picks which way it rocks.

GTAClone's `scripts/e2e_drive.js` drives one in a real scene through
`input.inject`, the same actions a keyboard feeds, and **the same harness passes
on desktop and in the Web player** — resting wheel height 0.398 against 0.403,
throttle, steering direction and the handbrake. It is keyed on elapsed time
rather than a frame count for exactly that reason: the two backends run this city
at frame rates an order of magnitude apart, and a harness that counted frames
measured a settled car on one and a mid-bounce one on the other.

A rigid body reports its velocity to scripts. `NodeRef.getVelocity`/`setVelocity`
answered only for a `CharacterBody` and returned a confident zero for everything
else, so a script asking a car how fast it was going was told nothing while it
was doing 30 m/s. Both now fall back to the body's own physics world; a character
still reads its solver's field rather than Jolt's readback.

A screen-space `WebCanvasNode` that fills the viewport reconciles the size it
was authored at with the window through `scaleMode` and
`referenceWidth`/`referenceHeight`. `Stretch` (default) resizes the canvas to
the window, so the document lays out at whatever resolution the player runs —
which is why absolute pixel geometry silently lands off-centre. `Fit` lays the
document out at the reference size and letterboxes the result, giving an author
one resolution to reason about. `Expand` keeps the reference on the constrained
axis and grants logical room on the other, filling the window without bars. A
mode without a reference behaves as `Stretch`. The node owns both halves of the
decision: rendering and pointer input read the same placement, so a click inside
a letterboxed image lands where it looks like it should and one on a bar misses
the canvas.

The CPU RmlUi sampler wraps texture coordinates when a compiled geometry leaves
[0,1], which is how a tiled decorator asks for repetition, and clamps otherwise.
`decorator: image(x.png repeat)`, `tiled-box` and `ninepatch` therefore tile.
The choice is per geometry rather than per sample on purpose: per sample nothing
distinguishes a glyph's right edge at exactly 1.0 from the start of a second
tile, and wrapping that edge to 0 tears text.

The right stick is bound by default to `LookLeft`/`LookRight`/`LookUp`/
`LookDown`, which `CameraFollow` reads as a rate in degrees per second on top of
the mouse's degrees per pixel. Scripts gain `input.bindKey`,
`input.bindGamepadButton` and `input.bindGamepadAxis` beside the existing
`rebind*` calls: rebinding unmaps the action first, which is right when
replacing a control and wrong when adding one, so an action wanting both a key
and a pad control could not be expressed from a script at all.

`CameraFollow` follows the same three-entry-point rule. Past the original rig
(distance, height, orbit, wall avoidance) it gained a vertical damping and dead
zone separate from the horizontal one — a platformer camera that rides every
jump shakes the whole screen —, look-ahead along the target's measured
velocity, auto-recentring behind its heading gated on delay and speed, and a
field of view that opens with speed. Occlusion is applied **after** smoothing,
not to the point it eases toward: pulling in has to be immediate or the camera
spends the easing frames inside the wall.

### 5.2 Input

The system aggregates analog/binary actions from bindings and stackable contexts.
Keyboard, mouse, delta/position, scroll, text and raw touch are sampled once per
frame.

On desktop, the first standard GLFW gamepad is detected with hotplug. On Web, the
backend polls the Gamepad API directly via Emscripten, accepts the `standard`
browser mapping, converts its buttons and sticks to the same semantic contract
and remaps the `[0, 1]` triggers into the GLFW `[-1, 1]` convention.
`GamepadInput` is announced only if `navigator.getGamepads` is usable at boot; an
absent controller stays distinct from an absent backend.

Buttons, sticks and triggers support rescaled deadzone, inversion and sensitivity
via `scale`. The default actions cover movement, jump, sprint, fire and aim.
Forces, edges and callbacks are aggregated across bindings: releasing one device
does not end an action held by another.

Runtime rebinding atomically replaces an action/context's controls via C++ or
QuickJS: `input.rebindKey`, `rebindMouse`, `rebindGamepadButton`,
`rebindGamepadAxis` and `rebindTouch`. `input.exportProfile(name)` produces a
schema-1 JSON with named controls, without transient frame state;
`input.applyProfile(json)` fully validates the document before replacing the
bindings. A game persists this string in `storage.prefs` then reapplies it at
boot. Future schema, unknown control, out-of-bounds identifier, deadzone outside
`[0, 0.99]`, non-finite value and more than 2048 entries are rejected without
modifying the active profile.

On Web, four Emscripten callbacks attached to the canvas actually feed
start/move/end/cancel; `TouchInput` is announced only if their installation
succeeds. `Press`, `Tap` and the four directional swipes bind to normalized
`[0, 1]` zones, independent of resolution. The swipe threshold is configurable,
the gestures are one-frame impulses and the hold stays active as long as the
contact is present in its zone. These bindings are part of the serialized profile
and are available in C++ as well as via `input.rebindTouch`.

`Input::lastActiveDevice` / `input.lastActiveDevice()` publishes `none`,
`keyboard-mouse`, `gamepad` or `touch`. Recency is based on transitions, not on a
held control; sticks and triggers filter idle drift. Adaptive prompts consume
this data on the game side: a UI script reads `input.lastActiveDevice()` and sets
the current binding's label (WitnessGame's HUD shows `Move: WASD` /
`Move: Left Stick` / `Move: Swipe`, keyboard default before any activity). For
proofs without hardware, `Input::injectDeviceActivity` /
`input.injectDevice(name)` simulate a device's activity — reserved for tests/CI,
like `injectAction`; later real activity naturally takes over.

V1 promises neither local multiplayer nor per-player device selection. On Web,
`Input::rumble` / `input.rumble(low, high, durationMs)` strictly uses the active
pad's W3C `GamepadHapticActuator` with `dual-rumble`; `stopRumble` calls `reset`.
The magnitudes are bounded to `[0, 1]`, the duration to 5 seconds, and the API
returns `false` if the pad or the effect is missing. Desktop returns `false`,
because GLFW 3.x exposes no standard haptics. `Input::injectAction` and
`input.inject` are reserved for tests/CI.

### 5.3 Audio

`AudioManager` uses miniaudio on desktop and Web Audio in the Web player. JS
plays a project alias with `audio.play(alias)`. Browsers require a user gesture
before starting sound; the player therefore starts muted and unlocks on the first
click/touch.

### 5.4 Player storage

The `storage.*` JS API persists opaque strings per slot: the game serializes its
own state (`JSON.stringify`) and the engine only stores the string. The
`PlayerStorage` service (pure filesystem, tested headless by
`saida_player_storage_tests`, shared desktop/runtime/Web player) carries the
durability:

- **Two separate namespaces.** `storage.save/load/has/remove/info/list` operate
  on *progression* (`saves/<slot>.json`); `storage.prefs.*` on *preferences*
  (`prefs/<slot>.json`). Erasing a save does not touch the settings and vice
  versa. The name respects `[A-Za-z0-9_-]{1,64}`.
- **Versioned envelope.** Each slot is written in a JSON envelope
  `{schema, version, __saidaStore, kind, dataVersion, savedAt, bytes, payload}`
  (current schema 1). `storage.save(slot, json, dataVersion?)` accepts an
  application version belonging to the game, without modifying the engine format.
  Any envelope whose schema is not exactly the current schema or whose
  `schema`≠`version` is rejected via the shared guard
  `format::schemaEnvelopeError`: `load` returns `null` and sets
  `storage.lastError()` instead of misreading the data.
- **Strict format.** A save without a complete and current envelope is invalid.
  The runtime attempts neither historical detection nor implicit promotion.
- **Slot metadata.** `storage.info(slot)` returns
  `{kind, bytes, savedAt, dataVersion, schema}` without reading the payload;
  `storage.list()` enumerates a namespace's slots.
- **Quotas and typed errors.** Per-slot budget (1 MiB), per-namespace (16 MiB)
  and number of slots (256) are enforced; an overshoot fails `false` with a
  consultable status (`storage.lastError()` → `{status, message}`, `status`
  among `invalid_slot`/`quota_exceeded`/`not_found`/`corrupt`/`io_error`).

`storage.save` writes a temporary file in the same directory, forces the data to
storage then atomically replaces the destination (`writeFileAtomically`). The old
file stays intact if the write or the replacement fails, and failed temporaries
are deleted.

**Location (`core/Paths::userSaveRoot`).** A packaged game never writes next to
its exe (read-only Program Files, shared portable copy): its `saves/` and
`prefs/` live under the OS user data folder, keyed by the game's identity (its
name sanitized into a safe folder component), set at boot by the runtime
(`setSaveIdentity`). Precedence: (1) `$SAIDA_SAVE_DIR` (explicit override,
CI/tests/portable saves), (2) OS user dir
(`%APPDATA%\SaidaEngine\Games\<game>`, `$XDG_DATA_HOME`/`~/.local/share/...`,
`~/Library/Application Support/...`) keyed by identity, (3) fallback to the
project root when no identity is set (editor/dev) or it is unresolvable. Web uses
IDBFS/IndexedDB (`syncfs` flush after each durable mutation), mounted at the
project root by the shell — unchanged.

Outside this contract: the full async API needed for IDBFS/cloud save.

## 6. QuickJS JavaScript

### 6.1 Modules and lifecycle

A `ScriptBehaviour` loads `.js` or `.mjs`. The exported hooks are called by the
lifecycle. They are inspected once after loading: a hook of the wrong type is
reported and a module without a recognized hook produces a single warning. Script
autoloads share the same runtime.

Each JS entry has a 100 ms deadline. A drain is limited to 1024 jobs and
recursive microtask chains are interrupted. C++ callbacks convert their
exceptions into QuickJS errors.

The main script and all imports stay in the project's canonical root. Only `.js`
and `.mjs` are accepted; out-of-project absolutes, traversals and outbound
symlinks are rejected.

Scripts/modules and WebCanvas hot-reload transactionally: a new invalid version
does not replace the live context/document. Relative imports and actually opened
RmlUi dependencies participate in the tracking. There is no C++ DLL hot reload:
dynamic libstdc++ linkage is fragile on the current UCRT64 toolchain and the
build is static. C#/.NET is not retained in order to avoid a runtime, marshalling
and a second bindings ecosystem.

### 6.2 Script permission policy (V1)

The model is *capability-based*: a script has no ambient authority beyond the
globals the engine explicitly installs — `console` and the
`node/time/input/tree/assets/audio/physics/storage` capabilities (plus
`exportProperty`/`props` during the loading of a `ScriptBehaviour`). Concretely:

- no network (no `fetch`/socket), no OS, process or environment-variable access;
  quickjs-libc (`std`, `os`) is not linked;
- no filesystem: the only persistence is `storage`/`storage.prefs` (per
  slot/namespace quotas, typed errors); module imports are resolved only under
  the project's canonical root;
- interruptible time budget (100 ms deadline, 1024 jobs, interrupted microtasks)
  and protected callbacks — a hostile script can freeze its frame, not the
  process;
- any new capability must be added here AND in the allowlist of the
  `saida_js_permission_policy_tests` test, which locks the global surface by
  difference against a bare QuickJS context (an authority that appeared or
  disappeared fails the suite).

### 6.3 V1 candidate API

- `node`: name, position, translation, rotation, activation, deferred removal,
  groups, `on`, `emit`; on `UITextNode`, `setText/getText`.
  `getRotation()`/`setRotation(x, y, z, w | {x,y,z,w})` carry the transform's
  quaternion; a non-finite or zero-length value is rejected (`false`) rather than
  repaired, so a node never receives an orientation its caller did not ask for.
  Character control resolves like the gameplay bindings — the node itself, else
  the first `CharacterBody` below it: `setVelocity`/`getVelocity` write and read
  the velocity the engine integrates during the physics step, and
  `isOnFloor()`/`isOnSteepSlope()`/`groundNormal()` publish the floor state
  computed by that step. Without a `CharacterBody` the setter returns `false` and
  the getters answer zero/false/up, never an exception — the same fail-soft rule
  as the `physics` queries. A game therefore drives a full character controller
  from JavaScript alone, without a native behaviour. It can also drive the
  native one instead of replacing it: `characterMove(x, y)`, `characterJump()`,
  `characterReleaseJump()`, `characterLaunch(x, y, z)`,
  `characterImpulse(x, y, z)`, `characterFace(x, z, instant?)`,
  `characterSolver(enabled)` and `characterState()` reach the `Character`
  behaviour resolved the same way. `characterState()` returns one object —
  `grounded`, `jumping`, `skidding`, `speed`, `airTime`, `jumpsUsed`,
  `jumpChainIndex`, `facingYaw`, and `wantedX`/`wantedZ`, the stick already
  resolved against the camera — or `null` when the node carries no controller.
  This is what lets a game keep a tuned solver and add only the moves that are
  its own. The raycast vehicle is reached the same way and for the same reason:
  `vehicleDrive(throttle, steer)`, `vehicleBrake(v)`, `vehicleHandbrake(on)`,
  `vehicleInput(enabled)` and `vehicleState()` — the latter returning `speed`,
  `forwardSpeed`, `grounded`, `wheelsOnGround`, `throttle`, `steer`, `handbrake`
  and `readsInput`, or `null` when the node carries no vehicle. `vehicleInput`
  exists because a character and a vehicle read the **same** movement actions,
  so exactly one of them may be listening: a parked car left listening steers
  itself off the kerb whenever the player walks. Turning it off also releases
  whatever the keyboard was last holding, so a car cannot be handed over mid-
  throttle. This is the seam a driver plugs into, whether it is a player getting
  in or a traffic AI. The same five are on `NodeRef` as well as on `node`,
  because a driver is not always the vehicle's own script: a player picks one car
  out of the several within reach and drives *that* one, and a script per car
  would put every parked car in the city in a race to answer the same key press.
  Gameplay (the behaviour is
  resolved on the node, else the first descendant):
  `playClip(name, loop?, crossfade?)`/`currentClip()` (Animator),
  `setAnimFloat/setAnimBool/setAnimTrigger` (animation blackboard → drives a
  `.sgraph`), `playSequence()/stopSequence()` (SequenceDirector, reflected
  `sequenceEvent`/`sequenceFinished` signals), `setData/getData/hasData`
  (gameplay Blackboard, number/bool/string, `changed` signal); a target without a
  behaviour → false/null, never an exception. The Animator's `animationEvent` is
  a reflected signal subscribable via `on`.
- `time`: `delta`, `elapsed`, `wait`, `every`, `tween`, `cancel`.
- `input`: actions, forces, axes, vectors, mouse and test injection.
- `audio`: `play(alias)`.
- `tree`: scene change/reload, quit, pause, `autoload`, `firstInGroup`,
  `nodesInGroup` and `nodeById`.
- `NodeRef`: weak reference resolved by NodeId, usual node operations — rotation
  and character control included, at parity with `node` — `on/emit` cross-node
  signals and `call(exportName, ...args)` JSON-compatible to a `ScriptBehaviour`
  in another QuickJS context.
- `assets`: `load(path, priority)` and `stats()`; never a blocking load promise.
- `storage`: opaque progression slots (`save/load/has/remove/info/list`, `save`
  accepts an optional `dataVersion`), `storage.prefs` sub-object for preferences
  and `storage.lastError()` for the last typed failure; described in 5.4.
  Durability contract: visibility is synchronous (a `load` after `save` returns
  the value), durability is asynchronous — `storage.flush()` returns a Promise
  resolved `true` when the pending writes (saves AND prefs) are durable, `false`
  on failure, never rejected. Desktop: durable atomic writes as of `save`,
  resolution at the next microtask drain; Web: resolution via the `FS.syncfs`
  callback (IndexedDB); a future cloud backend slots in behind the same promise
  without changing the API.
- `physics`: `available()`;
  `raycast(origin, direction, maxDistance, opts?)` → `null` or
  `{point, normal, distance, node: NodeRef|null}`;
  `overlapSphere(center, radius, opts?)` → `[NodeRef...]`.
  `opts = {hitSensors?: bool, ignoreSelf?: bool}` — sensors excluded by default,
  the caller's own body (node or ancestor) ignored by default. **Known defect:
  `ignoreSelf` misses a `CharacterBody`.** The character's inner body — the one
  that exists so the controller is visible to broadphase, sensors and raycasts
  (5.1) — is not the `CollisionObject` body the option resolves, so a ray cast
  from a character controller hits its own capsule at distance 0 with a
  horizontal normal. It fails in exactly the case that matters, a controller
  probing ahead of itself, and the result is well-formed enough to be believed;
  tracked in [ROADMAP](ROADMAP.md). Without a physics world (no Play, no body),
  the queries answer "nothing" (`null`/empty list), never an error. Same surface
  on desktop and Web player.

Cross-node references become explicitly invalid when their NodeId disappears; no
JS pointer survives the destruction of a scene.

A removed stable API must live at least one version in deprecation with a warning
before removal.

## 7. Animation

The engine supports glTF/BVH, rigs, clips, cubic-spline interpolation,
retargeting, GPU skinning, animation graph/state machine, blend nodes,
blackboard, clip views, timelines and `.sseq` sequences. Timeline properties use
reflection and interpolate float, int, vec3, vec4 and quat.

Formats: `.sclip`, `.sgraph`, `.sretarget`, `.srig`, `.sseq`; the `.sanimc` cache
is internal. The reflected behaviour `SequenceDirector` plays a `.sseq` at
runtime: targets are resolved by name in the carrier node's scene (animation
track to the target node's Animator or a descendant's, property track
`Node.property` to a reflected property of the node or one of its behaviours), the
event track is relayed by the `sequenceEvent` signal then `sequenceFinished` at
the end of playback. The binding is fail-closed: an invalid sequence or a target
still absent after the resolution delay disables playback with logged
diagnostics, without emitting any signal. WitnessGame traverses a rigged
character with Idle/Walk, a locomotion graph and the `anim/intro.sseq` sequence
(totem clips, `intro_beat` event, Sun intensity) on desktop and Web.

The reflected behaviour `AnimGraph` applies a `.sgraph` to every `Animator`
below its node, and does nothing else. An `Animator` is built by the glTF import
rather than authored in a scene, so a scene cannot address one directly:
something authored has to reach down to it. `CharacterBehaviour` already did,
bundled with its own movement logic, which forced a game driving movement itself
— from scripts or another controller — to take that movement too. `AnimGraph` is
that half alone, with a single `graph` property. It is fail-closed and applies
once: the graph is requested through the AssetLoader, and the first `Animator`
that rejects it aborts the whole attempt with a logged diagnostic, because a
half-applied graph would leave some limbs on the graph and others on the
play()-by-name FSM. Parameters are then driven through the animation blackboard
(`setAnimFloat`/`setAnimBool`/`setAnimTrigger`), so a game animates a character
from JavaScript with no native behaviour of its own.

The `.srig/.sclip/.sgraph` assets are loaded by the AssetLoader without blocking
the frame. The runtime continues with its current state during `queued/loading`;
a character graph becomes the owner of playback only after successful loading,
validation and compilation.

Generalized SIMD, massive pose sharing and GPU crowds are deferred until
measurements justify them.

## 8. UI and WebCanvas

### 8.1 Model

RmlUi renders lightweight HTML/CSS documents in Screen Space or World Space.
`UICanvasNode` carries the document and the mode; text nodes and controls interact
with `UIInteractionSystem`. `WebCanvasNode` provides a targeted DOM/JS for game
UI, not a general-purpose browser.

A document must keep structure, style and behavior separate: `.rml` or HTML for
the DOM, local CSS and a project JS module. Layouts use flex, explicit sizes and
simple units; avoid unsupported CSS functions, network dependencies and
full-browser assumptions.

### 8.2 Expected authoring surface

- loading of document and stylesheets from the project;
- text, images, classes, attributes, click/hover/focus events;
- targeted DOM mutation, data binding and calls to QuickJS;
- hit-test consistent with viewport, DPI, resize and input capture;
- Screen Space for HUD/menus, World Space for 3D panels;
- serialization of paths/modes and Play/Stop/reload lifecycle.

The desktop bridge currently exposes the following browser subset:
`getElementById`, `querySelector(All)`, `body`, `documentElement`, selectors on
elements, `textContent`, `innerHTML/innerRML`, `id`, `classList`,
`style.setProperty/removeProperty`, `add/removeEventListener`, `click`, `focus`,
`blur`, `getBoundingClientRect`, offsets and client sizes.

**Authoring baseline — the engine user-agent stylesheet.** RmlUi ships no
default stylesheet and RCSS defaults `display` to `inline`, so a `<div>` that
does not declare its display is an inline box that silently discards `width`,
`height`, `padding` and `text-align` — markup written the way HTML is normally
written collapses into a left-aligned run of text with nothing logged. The
engine therefore merges its own baseline under every document it loads
(`RmlUiRuntime::userAgentStyleSheet`, embedded rather than shipped as a file so
it cannot go missing from a package):

```rcss
body, div, p, section, h1, h2, h3, h4, h5, h6 { display: block; }
img, button { display: inline-block; }
```

It is a contract, not a theme: it sets `display` and nothing else — no margins,
no fonts, no colours. It is merged *under* the document, so any rule the
document declares wins. Documents must be created through
`RmlUiRuntime::loadDocument`/`loadDocumentFromMemory`; a document loaded
straight from an `Rml::Context` does not get the baseline. Both halves are
proven by `saida_ui_corpus_tests` (the baseline applies and paints the declared
box; the same markup loaded bare still computes to `inline`; a document's own
`display` overrides it). Tags outside the list above keep RCSS's `inline`
default — extending the list is a contract change.

Reliable CSS: block, inline-block, flex, direction/align/justify/gap, margin,
padding, px/% sizes, position absolute, hover/active, classes, colors, borders,
backgrounds, fonts and line-height. `text-shadow` is filtered; vendor properties,
complex transforms, masks, filters and advanced compositing are not guaranteed. A
`.hidden` class may lose against a more specific selector; use for example
`#panel.hidden`.

A transparent HUD must not steal the game's clicks. Only native
`button/input/select/textarea` controls and `.ui-hit` elements capture the
pointer. Screen Space uses the docked viewport's real rectangle, not the whole
swapchain; World Space raycasts the plane and XR rays can query it. Script/image/
style paths are relative to the document.

The UI JavaScript must stay modular and use the engine APIs, without implicit
network or system access. Author content must not depend on ImGui.

### 8.3 Actual state

The RmlUi CPU backend is proven by the headless corpus `saida_ui_corpus_tests`
(no GPU): solid geometry, alpha blending (premultiplied vertex colors), default
font glyphs, project stylesheet loaded from disk with a filtered web property,
decoded project image, missing image → magenta checkerboard (same convention as
`ResourceManager::missingTexture`), `overflow:hidden` actually scissored, CSS
`transform`, resize and DPI ratio. A GPU/Vulkan RmlUi backend remains a future
optimization, not a condition if the CPU backend holds the V1 load.

That same backend is exposed headlessly as `saida_tool render-ui <document>
--project <dir>`, so a UI document is verifiable without a GPU, a window or a
human. `--out <png>` writes the rendered frame (default 1920x1080, `--size WxH`
to change it) through the engine's own PNG encoder (`core/PngWriter`: fixed
Huffman deflate, no third-party encoder, output a pure function of the pixels so
a frame is usable as a committed golden image). `--layout-json <file|->` writes
what the layout engine computed per element — tag, id, classes, computed
`display`, border box in image coordinates — plus every diagnostic RmlUi raised;
for a machine that is strictly more useful than the image, being assertable and
diffable. The document path is project-relative and confined exactly like every
other tool path (§6.2): absolute paths, parent traversal and symlink escapes are
refused before anything is read.

`saida_tool validate-ui <document> --project <dir>` lints a document on the
model of `validate-scene`: a JSON report, exit 0 when clean and 1 when not. It
reports four kinds, chosen because each is silent otherwise —
`rejected-declaration` (what RCSS refused, which it logs where nobody reads it),
`unresolved-asset` (a referenced image that could not be opened, which renders
the magenta checker), `inline-box-properties` (an element computing to
`display: inline` while carrying `width`, `height` or its own `text-align` —
always a bug, since RCSS discards them without a word), and
`fractional-rgba-alpha` (`rgba()` written with a 0-1 alpha; RCSS alpha is 0-255,
so the value parses as nothing and the *whole declaration* vanishes with no
diagnostic at all). The last is found by reading the document and the
stylesheets it pulled in, because a dropped declaration leaves no trace in the
live document; it is reported with `file:line` and the 0-255 value that was
meant. `text-align` is only reported when it differs from the parent's computed
value, so a legitimately inherited alignment is not flagged, and RmlUi's
anonymous `#text` elements are skipped — they are always inline and never
authored.

Both UI commands share `cli/UiDocumentSession`, which owns the headless
document: one runtime, one context, one document, engine output routed to
stderr, diagnostics and file dependencies recorded. The two must agree on what
opening a document means, or their verdicts would not be comparable.

The engine itself captures the composite the player sees — 3D, editor chrome
and UI in one image — with `--screenshot <png>`, accepted by both `SaidaEngine`
and the exported player. `render/FrameCapture` owns the staging buffer and
exposes only record/resolve; the copy is recorded while the swapchain image is
still a colour attachment, just before it transitions to Present. This requires
`VK_IMAGE_USAGE_TRANSFER_SRC_BIT` on the swapchain images, which the spec does
not guarantee: it is requested when `supportedUsageFlags` offers it, and
`Swapchain::supportsCapture()` reports its absence so a capture refuses instead
of writing an empty file. The channel order comes from the actual swapchain
format, and a format outside R8G8B8A8/B8G8R8A8 is refused rather than written
with swapped channels.

**Which frame is captured** is a separate concern, owned by
`render/CaptureScheduler` — no Vulkan, no window, no renderer, so the rule that
makes a capture reproducible is proven headlessly
(`saida_capture_scheduler_tests`). Two things decide it, and neither is visible
in the resulting PNG:

- `--after-frames N` counts frame N **of the settled sequence**, not of the
  process. The scheduler first waits until `ResourceManager::assetLoadsSettled()`
  reports nothing queued, in flight or waiting to be finalized into a cache; an
  asset landing on frame 3 in one run and frame 4 in the next would otherwise
  change what frame 3 shows. The wait is one-way — a load requested *by* the
  settled frames does not restart the count — and bounded by
  `--settle-timeout N` (default 600 frames), which abandons the capture with a
  diagnostic and exit 1 rather than photographing a half-loaded world.
  `--no-wait-assets` opts out, for an interactive screenshot that is explicitly
  not a golden image.
- `--fixed-step <seconds>` (default 1/60) hands every frame the same delta
  instead of the measured one, for `Time` *and* the application callback, and
  disables the `maxFps` throttle — pacing a fictional clock only makes the
  capture slower. `--fixed-step 0` restores the real clock.

**Where the capture is taken from** is the third flag pair, and it is what makes
a screenshot an instrument rather than a souvenir: `--camera-pos x,y,z` with
`--camera-look x,y,z` parks the camera and aims it, overriding the scene's own
cameras for the whole run. The defects that survive a structural check — a wheel
sunk into the road, a wall not meeting the floor — are visible only from
somewhere the gameplay camera never goes. The override is applied every frame
immediately before the scene is drawn, after the `CameraDirector` and the editor
have had their say, so nothing can win it back and the world streams for the
viewpoint that will actually be photographed.

Both flags are required together, each must parse as exactly three numbers, and
the two points must differ. Every one of those refusals replaces an image that
would look like a rendering bug: half a viewpoint aims a placed camera wherever
the scene happened to look, a short vector puts it at a partly-default position
(usually inside the floor), and a zero-length direction makes `lookAt` produce a
NaN orientation and a black frame. A rejected viewpoint fails before the loop is
entered, so nothing is drawn and no image is left behind — a run that exits
non-zero must not leave a plausible PNG a caller might read instead of the exit
code.

Because both defaults are the reproducible ones, `--screenshot` alone produces a
golden image. Its **resolution follows the window**, so a committed reference is
taken headless (`SAIDA_WINDOW_HIDDEN=1`); a capture from a visible window is a
different image size and compares as a size mismatch.

The comparison half is `saida_tool compare-png <actual.png> <expected.png>`:
exit 0 when the images match within the stated bounds, 1 when they differ, 2 on
bad invocation or I/O. It reports the number of differing pixels, the worst
per-channel delta and the bounding box holding the difference — that box is what
separates "the tonemap moved" from "one glyph changed" — and `--diff <png>`
writes the reference in grey with the differing pixels in magenta. Comparison is
exact by default; `--tolerance N` (per-channel) and `--max-different N` exist
because a GPU may differ from a software rasterizer in the last bit of a
channel, and both are echoed in the report so a permissive run cannot be
mistaken for a strict one. A size mismatch is its own verdict, reported before
any pixel is examined, so no tolerance can hide it.

A golden image is taken from the **exported player**, not the editor: the
player draws the game and nothing else, while the editor's live FPS and camera
overlays differ between two runs of the same command. Both surfaces accept the
same flags through one parser (`runtime/CaptureArgs.hpp`) — an image captured
from one and compared against the other would otherwise compare two policies.

`tools/witness_golden_image.sh` is the gate built on all of the above, and runs
in CI on every push and pull request: export WitnessGame, capture frame 30 of
the hub scene, compare against `tests/fixtures/golden/witness-hub.lavapipe.png`.
On failure the diff image, the capture and the reference are uploaded as an
artefact, so a red run is diagnosable without reproducing it.

The reference is keyed to **Lavapipe**, and the gate refuses any other backend
rather than comparing across two. The same build on an Intel Iris Xe differs
from llvmpipe by 69 268 of 230 400 pixels — a banding pattern over every lit
surface, worst channel delta 90, i.e. ordinary floating-point divergence between
two rasterizers. A tolerance wide enough to absorb that would hide any
regression worth catching, so a real GPU is not gated at all.

Within Lavapipe the capture is byte-identical across runs, but **not across Mesa
builds**: two Mesa versions rounding the last bit differently move 5 634 pixels
by a delta of 1. A tolerance of 1 absorbs exactly that, and it is not defensible
— multiplying the AO exponent by 1.01, a real renderer change, moves 2 175
pixels by a delta of 1. *Fewer* pixels than the cross-Mesa noise, at the same
magnitude. Neither the delta nor the pixel count separates a subtle regression
from a Mesa mismatch, so no threshold on a cross-build comparison catches one; a
tolerance of 1 was measured passing a deliberately changed renderer.

The comparison is therefore **exact**, and the reference is recorded on the CI
runner rather than a developer machine: **the CI run is the authoritative one**.
A local run on a different Mesa fails with every difference at delta 1, and the
script names that signature — it is what a Mesa mismatch looks like, and also
what a genuinely subtle change looks like, which is why it cannot be waved
through locally. When Mesa moves on the runner the gate goes red the same way,
and the reference is re-recorded from the failing run's uploaded frame,
deliberately.

`--record` rewrites the reference for an intended visual change; the gate proves
a frame did not *change*, never that it is *right*, so a re-recorded image must
be looked at before it is committed.

The `render-ui` command is **fail-closed**, which is a deliberate divergence from RmlUi's own
behavior: RmlUi warns about a malformed document or a refused declaration and
then renders whatever survived, so a broken document otherwise yields exit 0 and
a plausible picture. Any diagnostic raised while the document loads and lays out
therefore rejects it — exit 1, every complaint named on stderr, and **no file
written**. `--allow-warnings` is the explicit opt-out for a document whose author
has accepted its warnings. Engine log output is routed to stderr for the duration
of the render, so `--layout-json -` stays parseable on stdout.

The text HUD (`UICanvasNode`/`UITextNode`) is rasterized by the shared module
`ui/HudRasterizer`: desktop and Web player build the same markup and the same
RGBA8 pixel buffer via the CPU backend, then each platform composes it via its RHI
(Vulkan bindless quad, WebGPU texture+bindgroup) — that is the visual-parity
invariant applied to the HUD. On desktop, the non-text UI nodes
(color/image/button/toggle) stay bindless quads via `UIRenderer::traverseUI`; the
V1 Web player registers only `UICanvasNode`/`UITextNode` and rejects the other UI
nodes and `WebCanvasNode` as long as their Web backend is not ported.
`setText/getText` operate on the real nodes; WitnessGame reaches `[E2E] PASS` with
an actually rasterized HUD (`[HUD] rasterized N visible pixel(s)`) in the editor,
packaged desktop and Web.

The engine's default fonts (`ui/RmlUiRuntime`: `kEngineFonts`) are resolved by
file under `assets/fonts/` (packaged bundle or runtime root) then the dev
checkout; a required font that is absent is logged as an explicit error and a
total load failure is reported. The `BuildExporter` embeds these files under
`assets/fonts/` in the desktop and Web packages (NotoEmoji deliberately out of
the web bundle).

The screen HUD interaction (`UICanvasNode`/`UIInteractableNode`) has a single
canonical path, `ui/UIInteractionSystem`: hit-test of node rectangles (pivot
included, topmost wins), hover/press/click machine and input-capture decision. The
key contract — *a HUD does not steal the game's clicks* — is explicit: only an
active `UIInteractableNode` under the pointer captures the mouse
(`Input::setUiCapture`); a purely text/decorative HUD leaves the input to game
logic. Proven without a GPU by `saida_ui_interaction_tests` (hover and capture,
click press+release, click cancelled on drag-out, non-capturing text HUD,
transparent disabled button, topmost winner, inactive canvas). V1 decision:
keyboard focus, scroll and touch on the *canvas* interactables are not added, for
lack of a V1 surface — the Web player HUD is display-only
(`UICanvasNode`/`UITextNode` only, §8.3) and the interactive canvas UI is driven
with the mouse on desktop. Rich keyboard, scroll and touch already exist on
`WebCanvasNode` for desktop panels.

World Space (3D panel `WebCanvasNode`) intersects a ray with the panel's local
z=0 plane, bounded by its world dimensions, and maps the point into pixel space
(top-left origin, y downward). This geometry is isolated from the GPU-bound node
in `ui/WorldPanelGeometry` (`raycastWorldPanel`) — the same function serves the
mouse (`UIInteractionSystem`) and the XR ray (`XRRayInteractor`) — and proven
without a GPU by `saida_ui_worldspace_tests` (center, corners, y-down mapping,
out-of-bounds/parallel/behind/degenerate rejections, translated and rotated
panels). World-space rendering (GPU compositing) stays exercised on desktop, not
asserted in pixels.

The author contract (`.rml`/HTML structure, local CSS, project JS module) is the
one frozen in §8.2: the reliable CSS subset, the targeted DOM bridge and the
structure/style/behavior separation. The `saida_ui_corpus_tests` corpus locks the
rendering subset (project stylesheet, filtered unsupported web property, layout,
clipping) and `saida_ui_interaction_tests` the interaction semantics; these are
the contract's non-regression tests.

The DOM/JS bridge (`WebCanvasNode`) is a *targeted and explicitly enumerated*
surface (`installDocumentBindings`: `document` — browser subset of §8.2 — and
`tree`), with no ambient browser API: no `window`, `fetch`, `XMLHttpRequest` or
global timer is installed, and the context runs on the same *capability-based*
QuickJS as the scripts (§6.2: no quickjs-libc, no network/OS, imports confined to
the project root — surface locked by `saida_js_permission_policy_tests`). A test
surface dedicated to the WebCanvas context stays coupled to its GPU init.

Serialization and lifecycle: the HUD documents
(`UINode`/`UICanvasNode`/`UITextNode`) round-trip semantically in the headless
codec (proven by `saida_runtime_type_matrix_tests`), and WitnessGame proves the
Play/Stop/reload lifecycle — the HUD is restored after a desktop restart and a Web
reload. The transactional hot reload of `WebCanvasNode` documents
(`loadDocumentFromState`, keeps the old document if the new one fails) is a
desktop behavior exercised in the editor.

UI assets and AssetRegistry — V1 decision: UI documents reference fonts, images
and stylesheets by *project-relative path* (HTML/CSS/RML's natural model),
resolved and bounded to the root by the RmlUi file/texture interface, with visible
errors and a proven magenta-checkerboard fallback. The AssetRegistry (`AssetID`
identities) stays the engine/mesh asset system, not the author tag; routing the UI
through `AssetID` is deferred (the V1 corpus has no heavy UI assets — assumed
consequence: UI textures do not go through the `AssetLoader`'s LRU GPU budget,
like the KTX2 decision).

CPU vs GPU backend — V1 decision: no RmlUi GPU backend. The CPU rasterization of a
full 1080p HUD costs O(canvas surface) and is only paid on the frames where the
content changes (the rasterizer skips an identical HUD); `saida_ui_corpus_tests`
publishes this cost (Debug measurement) and the Witness harness's Release hitchMax
(~0.05 s under full load) stays bounded. A RmlUi GPU backend is a P2
optimization, re-evaluated if a HUD must re-rasterize full-screen every frame.

XR — declared fallback (§10, gate P0.3): the XR UI is not a V1 delivery surface;
its absence is an announced fallback, not a blocker for the gate.

The V1 level requires: robust fonts/assets, screen-space, world-space,
clipping/scissor, resize/DPI, keyboard/mouse/touch input, XR fallback, DOM/QuickJS
bridge, lifecycle, serialization, inspector, picking and test scenes.

Quality of an example: natural HTML/CSS/JS, JS state separate from the DOM,
normal layout, non-capturing transparent zones, demonstrated `click` and DOM
mutation, zero warning, editor/runtime operation and transactional hot reload that
keeps the old UI if the new one fails. To diagnose: `lastError`, RmlUi/QuickJS
logs, relative paths, rendered pixels/bbox, CSS cascade and the
`EditorUI::viewportPosition/Size` rectangle.

## 9. SaidaFX particles

SaidaFX has three levels: `ParticleSystemNode`, a `.saidafx` asset composed of
emitters/modules and a `ParticleFeature` registered in the renderer. The
reflected node exposes class `Simple/Fire/Magic/Rain/Snow/Smoke/Explosion`,
budget, spawn rate, lifetime, initial velocity/size, start/end colors, gravity,
radius, emissive, blend `Alpha/Additive`, looping/playing and `effectPath`. Slots:
`play`, `stop`, `burst`, `applyEffectPreset`, `loadEffect`; signal `finished`.

The V1 CPU path renders HDR billboards, rotation/stretch, alpha/additive, compacts
in one pass, reserves per emitter, reduces the cadence at distance and frustum
culls on desktop/stereo. The executed modules cover Point/Sphere/Disc/Box/Cone/
Ring shapes, burst, drag, noise/turbulence, attractor, size-end and stretch. The
templates live under `assets/fx`; the `QualityTier` budgets and overdraw/mobile/XR
warnings are exposed.

The GPU runtime has buffers, descriptors, a `deadIndices` freelist, counter reset,
host-visible upload, emit/sim dispatch and compute barriers; the shaders
`particle_emit.comp`, `particle_sim.comp` and desktop/multiview render exist. The
draw still uses the packed CPU buffer: upload from `ParticleFeature`, indirect
draw, buckets per blend and real GPU execution remain to be wired.

V1 does not depend on a full graph editor. Remaining as extensions: compilation of
the JSON modules into structs, `SubEmitter`, atlas/flipbook, alpha sorting, soft
particles, ribbons/trails, mesh particles, heat distortion, shockwave, decals,
light pulses, emitter/module editor with preview, finer LOD, half-resolution
smoke, global camera/XR limits and detailed stats. The LLM ergonomics target
preset creation, addition/module, parameter modification, saving and application.
Any evolution measures CPU/GPU, memory, overdraw and determinism and keeps the CPU
fallback.

## 10. XR

OpenXR manages session, swapchains, actions, multiview and SaidaXRTK: grab,
teleportation, abstract anchors, passthrough depending on the extension and hand
tracking `XR_EXT_hand_tracking`. Procedural hands serve as a fallback without an
asset.

The XR preview is a separate process `--xr` because OpenXR must create the Vulkan
device at startup. The test scene is `assets/scenes/XRSetup.scene` (passed via
`--xr --scene <path>`). Quest Link and the Meta/Oculus runtime must be active for
a Quest test.

Limits: multiview MSAA/resolve, XR ImGui overlay, real anchors backend and a
reproducible headsets/runtimes matrix not closed. The logs indicate hand-tracking
support and active/lost transitions; a compilation does not replace the hardware
test.

## 11. Editor, MCP and AI

The editor provides a scene tree, a reflected inspector, a file browser, gizmos,
Play/Stop and undo/redo. All the inspector's durable mutations go through undoable
commands (`SetPropertyCommand`: resolution by NodeId, before/after snapshots). The
lists (WebCanvas startup scripts) commit a full snapshot of the list, so the
indices stay consistent under LIFO undo/redo. The CollisionShape type change and
"Recompute from mesh" capture the full durable state (type + parameters);
replaying a state that enters Auto re-arms the detection (`resetAuto`), whereas
undo restores the parameters without re-arming it — the still-valid detection
cache keeps them frozen. Proven headless by `saida_editor_command_tests`.

Project renaming goes through `renameProjectDirectory`
(`src/project/ProjectRename.*`): the folder, the `.saidaproj` file (name and
`name` field) and the `hub.json` entry are modified together, each intermediate
step stays loadable and any failure restores the prior state. The name is
validated as a safe path component, a corrupt Hub registry or a non-current
project document rejects the operation, and `Project::load` now accepts the
project folder (resolution of the single `.saidaproj`) — the path the Hub stores
and passes to `--project`. The "Project Name" field in the editor settings is
read-only and refers to the Hub: a consistent rename requires the project closed
(renaming of the folder held open by the editor).

The native MCP exposes tools to agents. The target contract requires per-tool
permissions, validation, dry-run/diff, grouped transactions, snapshot/rollback
and audit. World model, skills and autonomous agents stay out of V1 as long as
these guardrails are not closed.

`write_cpp_behaviour` writes LLM behaviours under `src/generated/`. CMake globs
them into `saida_engine` and their registration goes through
`scene/ReflectedTypes.cpp`. This path is a privileged C++ write: it must stay
behind permissions, diff, validation, build and rollback.

### 11.1 Optional Python authoring plugin

`plugins/python-tools` is a standalone Python package for explicit, offline
authoring tasks such as generators, converters, audits and asset reports. It is
not linked to the engine, is absent from CMake, is never loaded by the editor or
runtime, and is never copied by game export. Removing the package or not
installing Python has no effect on a project, engine build or shipped player.

Opt-in state lives in `saida.tools.toml`, separate from `.saidaproj`. Manifest
schema 1 is exact and declares named tools, typed parameters, input patterns,
output patterns, cache policy, and exactly one Python `module:function` entry or
argument-vector command. Commands run without a shell. Tools run only after an
explicit CLI invocation; project recipes are trusted host code, not sandboxed
gameplay code.

Declared outputs are written to `.saida/python-tools/staging/` and committed
with backup/rollback. Content-addressed receipts under
`.saida/python-tools/cache/` cover the tool contract, typed parameters, inputs
and Python entry source. These directories are authoring cache only and are not
project or runtime formats.

The package has no mandatory third-party dependency. Its optional subprocess
adapter can discover `saida_tool` and consume native JSON contracts for engine
description, validation and inspection. The adapter does not embed or import
engine code, and recipes that do not request it work without any engine binary.
Project recipes remain ordinary trusted Python and may use dependencies or
external programs from their own authoring environment; this does not add those
dependencies to the plugin core or engine. Machine consumers use the CLI's
JSON/JSONL diagnostics and result events.

## 12. Export and packaging

`BuildExporter` and `saida_tool export-game` produce a desktop package with an
editor-less runtime, project, scene, assets, shaders and `game.saida`. The
version, company, name and icon fields patch VERSIONINFO and RT_GROUP_ICON. The
Windows copy explicitly walks the trees, overwrites regular files and rejects
symlinks/special files.

The editor's Build click is automatable: `SaidaEngine --project <p>
--build <out> [--build-platform web]` runs exactly the code of the Build dialog's
button (`EditorUI::executeBuild`, same state defaults as opening the dialog — the
default main scene is the project's `mainScene`), logs `[BUILD] PASS/FAIL` and
returns the verdict as an exit code. `tools/witness_editor_build.sh` builds
WitnessGame through this path and requires the full E2E run + restart on the
produced artifact. CI keeps this exact path on a clean Windows runner with a
hidden GLFW window and the Mesa/Lavapipe software ICD explicitly selected; it
therefore does not depend on the runner's GPU.

The editor's Play mode is also automatable with `SaidaEngine --project <p>
--play`. It triggers the same deferred transition as the Play button;
`tools/witness_editor_play.sh` uses it on a pristine copy of WitnessGame and
requires gameplay, HUD and save+HUD restoration on the second launch.

`SaidaEngine --project <p> --screenshot <png> [--after-frames N]` writes the
presented frame and exits, non-zero when the capture failed (§8.3). It combines
with `--play` and `SAIDA_WINDOW_HIDDEN=1`, so a recipe can capture what a driven
session actually displayed rather than asserting on logs alone.

Recipes can add `--test-autoload NAME=script.js` without rewriting the
`.saidaproj` or the artifact. This autoload stays ephemeral, limited to a simple
name, to an existing `.js/.mjs` file and to the project's canonical root. The Web
player receives the same argument via the URL parameter `test-autoload`. Thus the
harnesses run exactly the archived bytes, and not a package modified after export.

`tools/witness_release_candidate.ps1` is the single P0.1 recipe. From a clean
worktree, it builds/verifies native and Web player, calls the real editor Build
for Windows and Web, rejects saves in the packages, archives the outputs and
produces `release-manifest.json`: engine SHA, dirty state, SHA-256 and size of the
archives and the installer, plus the hashed inventory of each file and the Windows
symbol bundle. The ZIPs are written in ordinal order with a unique timestamp
derived from the commit; ambiguous paths, symlinks and reparse points are
rejected, then each entry is re-verified against the stage. The scripts
`verify_witness_windows.ps1` and `verify_witness_web.ps1` re-verify the archive
before extraction. The first runs gameplay/UI then save/UI on restart; the second
checks COOP/COEP and WASM MIME, launches Chrome or Edge and collects an automatic
verdict via the local server. No engine checkout, MSYS2 or SDK is required by the
Windows proof on a pristine machine.

`build_witness_installer.ps1` compiles the same stage with NSIS 3.12+ into a
per-user installer. Before signing, its output is byte-reproducible: ordinally
sorted payload, timestamps from the commit, exact DLL closure and SHA-256
inventory. The uninstaller removes each inventoried file, the regenerable caches
`asset_registry.local.json`/`pipeline_cache.bin`, then only the folders that
became empty; it does not do a blind recursive deletion of the chosen folder.
`verify_witness_installer.ps1` checks the installer's SHA, installs silently into
an isolated folder, compares the exact payload, can run gameplay + restart, then
requires a clean uninstall. CI builds the same bytes twice, compares their SHA and
publishes the bundle under a name containing the commit. Authenticode signing is
deliberately separate: it modifies the bytes and requires the publishing key.

The Web package embeds player, shaders and boot files under MEMFS. The PNG/JPG
textures, OBJ and `.srig/.sclip/.sgraph` assets stay out of the preload and are
fetched on demand; scenes, scripts and glTF/GLB stay at boot.

WitnessGame is the vertical corpus: scenes, scripts, signals, physics, animation,
audio, UI, save/load and scene change. The desktop harness injects the actions via
an autoload and requires `[E2E] PASS`. On 2026-07-16, editor Play and desktop
export/runtime are PASS, restart included. The Web package loads and renders the
RmlUi HUD; its harness also reaches `[E2E] PASS` over 16 cycles then `RESTART
PASS` after reload. The three paths verify the HUD text before/after collection
and after save restoration.

The project contains `hub.scene` (CharacterBody player, CameraFollow, savepoint,
door and `SeqStatue` totem driven by a `SequenceDirector` that plays
`anim/intro.sseq` on autoplay) and `arena.scene` (three Area/Rotator/particle
relics, RigidBody crates and a return door). `GameState` owns the living state and
persists `saves/witness.json`; pickups, HUD, savepoint and harnesses call it via
`tree.autoload`/`NodeRef.call`, without using the file as a bus. The glTF totem has
three bones, Idle/Walk clips and the `anim/locomotion.sgraph` graph. The
`pickup.ogg` and `save.ogg` sounds are project aliases. The scenes regenerate via
`gen_witness.py`, the character via `gen_character.py`; modify the generators, not
their outputs.

Regressions now covered: reflected Area signals, JS storage, script autoloads,
cross-context JSON calls, groups/NodeId/cross-node signals, export shader path,
`setText/getText`, animation/audio, headless injection, script resolution from the
project root, Character inner body for triggers, cache refresh after
`changeScene/queueFree`, collider gizmos on the docked viewport, `?smoke` timer for
a hidden tab, WASM 4 MiB/QuickJS 256 KiB stack, Windows copy, QuickJS sandbox,
`.sseq` sequence traversal (event + end, desktop and Web), re-import of the same
glTF into a scene without invalidating the rigs/clips of already-attached
Animators, automated editor Build click (`--build`) and progression restored after
restart (second editor/desktop process on `saves/`, browser reload on IDBFS), as
well as automated editor Play (`--play`). To watch: unreproduced viewport halos and
autoload dispatch still duplicated between `Engine::mountWorld` and the Web player.

A release still requires the Authenticode signature of the installer with the
publishing key. The reproducible archive and installer, the DLL closure, the crash
logs with symbols, the SBOM, the notices, the content inventory, the rollback and
the immutable hashes are now produced or documented.

Each desktop executable installs `core/CrashReporter` before the engine boot. A
caught fatal exception writes a text report and, under Windows, a minidump into
the OS user data folder `SaidaEngine/CrashReports/<product>`; `SAIDA_CRASH_DIR`
provides the tests/CI override. The report contains product, UTC timestamp,
PID/TID, executable, reason/code/address, build commit, recent logs accessible
without waiting on the logger's mutex, module base/RVA for symbolization and the
name of the corresponding symbols artifact.

`tools/package_release_symbols.ps1` extracts the `.dbg` symbols of the four
`RelWithDebInfo` executables with GNU objcopy, strips the distributable copies,
writes `.gnu_debuglink` into them, pins the PE timestamp to the commit and produces
an exact manifest of sizes/SHA-256. The standalone verifier rejects a wrong hash, a
wrong link, a `.debug_info` section or an unexpected extra file. CI publishes the
bundle under `windows-symbols-<SHA>`.

`tools/validate_windows_dependencies.ps1` closes the PE dependencies of each entry
point and its local DLLs: mandatory `pei-x86-64` format, system import explicitly
authorized or a DLL present in the bundle, name collision and missing dependency
rejected. `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` are
forbidden because the UCRT64 contract links them statically. The deterministic
report enters the symbols bundle and the Windows Witness archive. The internal
PE64 reader bounds each offset/RVA and walks the normal and deferred import tables;
it therefore also covers the executable whose VERSIONINFO/icon were rewritten by
the Windows API, without depending on a third-party disassembler accepting that
rewrite.

## 13. V1 persistent formats

| Surface | Schema | Policy |
|---|---:|---|
| `game.saida` | 1 | exact schema required |
| `.saidaproj` | 1 | exact schema required |
| `asset_registry.json` | 2 | exact schema required, stable AssetID |
| `.scene` | 2 | exact schema required |
| `.saidascenario` | 1 | exact schema required |
| `.sclip` | 1 | exact schema required |
| `.sgraph` | 2 | exact schema required |
| `.sretarget` | 2 | exact schema required |
| `.srig` | 1 | exact schema required |
| `.sseq` | 1 | exact schema required |
| `.sanimc` | internal | regenerable cache |
| `asset_registry.local.json` | internal | regenerable local cache |
| `pipeline_cache.bin` | internal | regenerable GPU cache |

Every durable document must contain `schema` and `version`, integers, equal and
strictly identical to the current version of its surface. Any other form is
rejected without rewriting — **except `asset_registry.json`, which is rewritten
today**. `Project::load` ignores the result of `AssetRegistry::load` and runs
`sync()` then `save()` regardless (`src/project/Project.cpp`), so a registry the
guard refused is replaced by a fresh scan: every asset is re-registered with a
new random id, and the old file is overwritten. Opening a project whose registry
predates the current schema therefore loses every stored AssetID, and the
documents that reference them — a scene's `skyboxTexture`, a mesh reference —
keep pointing at ids that no longer exist. The rejection is logged; the damage
downstream is not. This is a known divergence from the policy above, tracked in
[ROADMAP](ROADMAP.md). The fixtures under `tests/fixtures/v1-format` are
immutable and loaded by `saida_v1_format_corpus_tests`. The `fold-determinism`
fixture proves a byte-identical Windows/Linux fold on its corpus, not the
exhaustive equivalence of all scenes.

Before stable publication, a format change directly replaces the schema, its
producers and its corpus in the next immutable pre-release: no migration between
betas is promised. Once a stable version is shipped, any new policy will have to
be decided explicitly. Public stability also requires a cross-runtime round-trip
corpus and a release manifest binding the hashes of the Web player, authoring
WASM, headless binary and formats.

`tools/engine_release_manifest.ps1` produces this release manifest
(`build/release/engine/release-manifest.json`, schema 1): the engine commit, the
format versions read from `saida_tool describe-engine` (the `formats` section is
their single source), and the SHA-256 of `saida_tool`, the desktop runtime, the
Web player, the authoring WASM, the authoring runtime and each immutable fixture.
It also includes the exact inventory of the compliance bundle: SPDX 2.3 SBOM,
GPL/third-party notices, hashed assets/models and a manifest of their sources, as
well as the stripped Windows executables and their versioned symbols.
`tools/verify_engine_release.ps1` recomputes each hash, rejects any file added or
missing from these bundles and re-compares the versions to the tool; it fails at
the slightest byte, inventory or version discrepancy. The Saida platform pins this
manifest to forbid any divergence between its Docker tool, its served Web bundle,
its diagnostics, its licenses and its fixtures.

`tools/generate_release_compliance.ps1` re-reads the two reviewed entries
`compliance/components.json` and `compliance/assets.json`. The check is
fail-closed: each `third_party` root must be declared exactly once, each tracked
asset must have a license, provenance and distribution decision, and no
`NOASSERTION` asset can be distributed. Since the open-source purge of 2026-07-21,
the repository no longer contains any non-distributable asset: the old projects
without provenance (`GTAClone/`, `MyGame/`) and the CC-BY-NC DamagedHelmet are
removed — 18 tracked assets, 18 distributable, 0 excluded.

The GPU/OS/browser matrix, the exclusions and the retirement procedure are
published in [§17](#17-support-promotion-and-retirement-of-a-release). Promotion is
done by manifest, commit SHA and immutable digests; `latest` is never a release
identity.

Current immutable inventory: `project_v1.saidaproj`, `asset_registry_v1.json`,
`asset_registry_v2.json`, `witness_v2_asset_registry.json`,
`scene_v2.scene`, `scenario_v1.saidascenario`, `game_v1.saida` and the frozen
witness game `witness_v1.saidaproj`, `witness_v1_asset_registry.json`,
`witness_v1_hub.scene`, `witness_v1_arena.scene` — exact copies of WitnessGame's
durable artifacts loaded by their real loaders (the UI HUD, the physics and the
V1 scene types validated headless). Never modify or regenerate these files; a
following published version adds `witness_v2_*`. The test also verifies that
loading changes no source byte.

The `fold-determinism` corpus contains `base.json`, `ops.json` and `expected.json`
produced under Windows. It covers set_property, create_node, set_transform with
non-trivial floats/quaternion, reparent, rename and scene setting. Linux must
produce a byte-identical output. `expected.json` regenerates only with a format
bump, never to mask a divergence.

## 14. Consolidated known limits

- V1 is available only as a public beta; no beta, local badge or CI result
  equals stable public qualification.
- Web player: WebGPU mandatory, HTTP mandatory, UI limited to the
  `UICanvasNode`/`UITextNode` HUD, WebCanvas absent, gamepads without the
  `standard` browser mapping ignored, advanced touch UI not proven, MSAA absent.
- Web audio subject to the user gesture.
- One Emscripten runtime/canvas per page; build not modularized.
- Native/headless/Web registries explicitly matrixed; the advanced UI stays out of
  some folds.
- External V1 SaidaOp producers must emit `opVersion: 2` and the `NodeId`; the
  historical by-name operations are deliberately rejected.
- Physics queries (filtered raycast, overlapSphere), V1 joints
  (Fixed/Point/Hinge) and animation/graph/sequence/blackboard bindings shipped
  with JS desktop/Web parity; advanced constraints (slider, cone, motors) and the
  extended animation API (scrub, JS root motion) stay P1.
- A packaged game's saves under the OS user folder (keyed by the game's identity,
  override `$SAIDA_SAVE_DIR`); editor/dev stay under the project root. Versioned
  envelope, metadata, progression/preferences namespaces, quotas and the async
  durability contract (`storage.flush()` → Promise) in place; only an actual cloud
  backend remains future.
- Mid-scene GPU budget with measured LRU, rig/anim sweep, stable glTF identities
  and rejection of corrupt content in place; Web fetch/IDBFS streaming, explicit
  tangent policy and meshopt UI export in place; the web package streams
  textures/OBJ and `.srig/.sclip/.sgraph` on demand (manifest schema 2, async
  fetch on MEMFS miss), scenes/scripts/glTF staying preloaded (MikkTSpace P1,
  KTX2/Basis P2 by decision).
- Point-light cubemap shadows and persistent lightmaps absent.
- XR without multiview MSAA, overlay and a validated hardware matrix.
- Authenticode signature of the installer not proven; it requires the publishing
  key and a qualification of the signed bytes.
- Recursive closure of the x64 DLL imports proven; the effective availability of
  Vulkan 1.3 remains a machine prerequisite.
- Windows crash reporter with minidump and a deterministic symbols bundle tied to
  the commit; remote collection of the reports out of the engine's scope.
- Licenses, notices and SBOM generated in fail-closed mode; since the open-source
  purge of 2026-07-21, every tracked asset in the repository is distributable
  under its declared license (the old projects without provenance and the
  CC-BY-NC DamagedHelmet are removed).

## 15. Public positioning

Honest positioning: a local-first C++17/Vulkan/WebGPU engine, an
editor drivable by structured MCP, a Web player and a witness game in public
beta. The
in-process MCP server and its stdio bridge exist; QuickJS/UI hot reload is partial;
there is neither a Lua promise nor a general C++ hot reload.

Do not claim before proofs: performance superiority, Web parity, third-party
project sandbox, stable compatibility, full generation from a prompt or production
XR support. A public demo must start from a signed tag, show MCP diff/validation,
the same project in the editor then the real desktop/Web exports, publish limits
and hashes, and be reproducible by a third party. Schedule, view or star goals are
not product guarantees.

## 16. Reference verification

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/saida_tool.exe describe-engine
./tools/witness_e2e.sh
./tools/witness_editor_play.sh
./tools/witness_editor_build.sh
./tools/witness_web_stage.sh
.\tools\witness_release_candidate.ps1
```

`witness_e2e.sh` launches the artifact twice: the second launch must produce
`[E2E] RESTART PASS` (progression restored from `saves/`). On the Web side,
reloading the page after an `[E2E] PASS` must produce the same verdict from IDBFS.

After a change to snapshot, SaidaOp, manifest, registry, scripting or shared
input: also rebuild `build-authoring-wasm` and `build-web-player`. A release proof
is obtained from a clean commit, with versioned artifacts; a local result stays a
development proof.

The historical probes under `web/spike` stay useful to isolate the toolchain:
`hello.cpp` proves emcc+Node, `webgpu_probe.cpp` the emdawnwebgpu link and
`spike.cpp` GLFW+canvas+WebGPU+rAF. Example:

```sh
emcc hello.cpp -O2 -o hello.js && node hello.js
emcc spike.cpp -O2 --use-port=emdawnwebgpu -sUSE_GLFW=3 -o index.html
python -m http.server 8080
```

The spike's expected result is an animated clear and the device/surface ready log;
it deliberately validates neither the engine's geometry nor its shaders.

## 17. Support, promotion and retirement of a release

Operational release guide. The checklist of what remains to be done is in
[ROADMAP.md](ROADMAP.md).

The public signing roles, privacy statement and trusted-build constraints are
declared in [CODE_SIGNING_POLICY.md](CODE_SIGNING_POLICY.md). The SignPath
release integration and publicly trusted certificate are not provisioned yet;
until they are, Windows installers remain explicitly unsigned and unqualified.

The raw `v1.0.0-beta.2` `SaidaEngine.exe` is a developer artifact rather than a
portable distribution: the editor build resolves shaders, fonts and branding
through absolute configure-time source/build paths, and the executable has no
Windows VERSIONINFO resource. The Open Project dialog inherits the same flaw: it
scans `SAIDA_PROJECT_ROOT` (the engine checkout) instead of the Hub registry, so
on a machine without the source tree it lists nothing and says nothing. A
qualified editor package must resolve resources relative to its executable,
discover projects through a path that exists on the user's machine, ship its
dependency closure and carry consistent product/version metadata before it is
submitted for signing.

### 17.1 V1 support matrix

A platform is supported only if the exact bundle of a clean release, identified by
its commit and its manifest, passes the indicated verifications.

| Surface | Supported target | Prerequisites | Blocking proof |
|---|---|---|---|
| Desktop editor and player | Windows 11 x64 | Vulkan 1.3 GPU and driver; system UCRT and shipped `glfw3.dll` | UCRT64 build, full CTest, compatibility corpus, Witness exported then run + restart, exact install and uninstall |
| Web player | recent stable Chrome and Edge on desktop | WebGPU enabled, HTTP(S) secure context, COOP/COEP, `application/wasm` MIME, IndexedDB | Emscripten build, runtime contract, Witness run + restart in CI Chrome; external Chrome and Edge recipe |
| Headless tool `saida_tool` | Debian 12 x64, glibc 2.36 | no GPU surface required for validate/fold/export | clean Debian container build, full CTest and byte-identical Windows/Linux fold |
| Authoring WASM | desktop browsers of the Web player line | WebAssembly, ES modules and a host conforming to the authoring contract | Emscripten build and blocking Node smoke |

The published Vulkan target is 1.3. The fact that the renderer can negotiate
certain optional features or an earlier loader version does not constitute a V1
support promise.

The Windows reference proofs were run on Windows 11 x64 with an Intel Iris Xe GPU.
Chrome and Edge both validated the Web bundle on a machine without an engine
checkout, MSYS2 or SDK. The exact versions of the browser, of the emsdk and the
commit must be kept in each new release's build identity; an old proof does not
automatically qualify a future version.

### 17.2 Unsupported or unqualified

The following surfaces must not be announced as supported in V1 until a dedicated
proof has closed their gate:

- Linux desktop editor or player;
- macOS;
- Firefox, Safari and mobile browsers;
- WebGPU disabled, `file://`, a server without COOP/COEP or a wrong WASM MIME;
- production XR, headsets, controllers and hand tracking;
- visual adaptation of prompts to physical Xbox/PlayStation controllers;
- desktop haptics, not exposed by GLFW 3.x.

An absent backend must stay visible in `PlatformCaps` and fail or degrade
explicitly. It cannot be reclassified as supported from a compilation alone.

### 17.3 Identity and promotion

A release's identity is the file
`build/release/engine/release-manifest.json`, not a mutable name:

1. start from a clean commit and keep its full SHA;
2. generate the engine manifest and the compliance bundle;
3. verify each file with `tools/verify_engine_release.ps1`;
4. verify the Witness archive and installer, then sign the installer with the
   publishing key and inventory the SHA of the signed bytes;
5. keep the CI artifacts whose name contains this SHA;
6. promote the platform and the container image by SHA or immutable digest;
7. use `latest` only as a convenient alias, never as a proof or as the sole
   deployment reference.

A release must never be rebuilt under the same identity. Any byte difference
requires a new commit, a new manifest and a new qualification.

### 17.4 Retirement and return to the previous version

In case of a regression or an incident:

1. immediately freeze the promotion of the affected SHA and collect the manifest,
   the logs, the symbols and the deployed digests;
2. mark the GitHub release as retired and publish a notice indicating the affected
   platforms, formats and versions;
3. stop serving or installing this SHA; never replace its artifacts in place;
4. re-pin each consumer to the manifest and the digest of the last qualified
   release;
5. replay `tools/verify_engine_release.ps1` as well as Witness run + restart on
   this version before reopening traffic;
6. keep the retired release and its proofs in audit access, but out of the normal
   installation channels;
7. fix forward with a new identity and redo the full qualification.

Rolling back a binary does not automatically downgrade the data. If a release
wrote a schema the previous version rejects, restore a compatible save or publish
a forward fix; never silently rewrite the snapshots, projects or saves. The
revocation of a signing certificate is a distinct operation, reserved for a key or
trust-chain compromise.

Under Windows, attach the `.crash.log` and the `.dmp` from
`%LOCALAPPDATA%\SaidaEngine\CrashReports\<product>\`. The log's `symbolsArtifact`
field designates the immutable CI bundle to download; verify it with
`verify_release_symbols.ps1` before any analysis.

### 17.5 Content and licenses

`tools/generate_release_compliance.ps1` produces the SPDX 2.3 SBOM, the notices,
the hashed inventory of the assets/models and their manifest. The
`distribution: false` entries would be excluded from the V1 bundles; since the
open-source purge of 2026-07-21 (removal of the legacy projects without provenance
and of the CC-BY-NC DamagedHelmet), the repository no longer contains any — every
tracked asset is distributable under its declared license.

The compliance bundle is included in the engine manifest and in the Witness
archives. Adding a root under `third_party` or an asset of a tracked extension
without an explicit decision fails the generation.
