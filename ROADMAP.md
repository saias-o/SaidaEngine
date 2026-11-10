# SaidaEngine — Roadmap

This file is the engine's **single backlog**: everything
that remains to be done, deferred or decided for later. It does not describe
what exists — the technical truth is in [SPEC.md](SPEC.md), and getting started
is in [README.md](README.md).

Rule: nothing is checked off here without the run, commit or exact corpus that
proves it. Closed work (V1 gates, V1 refactor) lives in the Git history and in
the corresponding contracts of `SPEC.md`.

## Ambient occlusion on indirect light only — 2026-10-22

- [ ] Preserve glTF `COLOR_0` vertex alpha across the vertex format and render
  paths. RGB import supports float/normalized integer VEC3/VEC4; non-opaque
  vertex alpha currently emits an explicit warning (SPEC, Media formats).

- [ ] The 2026-10-22 MX450 AO fixture added 3.5 ms (19.1 to 22.5 ms);
  the depth pre-pass is most of it: it costs 3.0 to 3.6 ms and saves 2.5 to
  3.8 ms of the opaque pass from one run to the next, so it does not always
  pay for itself. Measure where its time goes before changing it.
- [ ] XR: the AO path (multiview pre-pass, one AO layer per eye) is built and
  shares the desktop code, but no headset has run it yet.

- [ ] Fix what `SAIDA_VULKAN_VALIDATION=1` still reports outside AO/GI:
  the world-space WebCanvas pipeline binds the texture array at set 0 where its
  vertex shader expects `cam` (isolated by logging each shader pair during
  the GI phase-0 audit). Validation also reports legacy device-layer enablement,
  sampled-image update-after-bind enablement and descriptor-pool type warnings.
  GI's fragment-storage and first-use image defects are tracked in section 5.
- [ ] `saida_rain_surface_tests --gpu` fails "the horizon band dissolves into
  the haze" on the MX450, identically at bb57e40, before this work: with
  `fogSkyDistance` 0 the sampled band, just above the horizon, is already pale
  (0.894), within the skybox's 1–6 degree horizon match, so it is no clear-sky
  reference. Its measurements are lost on failure: `printf` is not flushed
  before `abort`.

## Independent native projects — 2026-10-18

- [ ] Provide a relocatable installed native SDK with compiler/ABI identity.
- [ ] Qualify project-native C++ on Web; stock Web export currently rejects it.
- [ ] Support native-code reload without closing/rebuilding the project editor.

## Large streamed-scene frame cost — MX450 audit, 2026-10-12

- [ ] Isolate and reduce procedural terrain submission/sampling work without
  changing relief, surveyed tile holes or morphing/seam rules. Repeat A/B
  measurements after the implemented whole-cell hole rejection (SPEC 4.1);
  the 18–19 ms spans from the original audit are not a current baseline.
  Confirm exclusive savings because pipeline timestamp intervals overlap.

## Ragdolls, reach and attachment — 2026-10-13

- [ ] Build and verify the Web player and authoring WASM with these sources
  (no Emscripten SDK on the machine that made them).
- [ ] Getting up from a ragdoll is a pose blend; recorded get-up clips or an
  active ragdoll driving to a pose (Jolt motors) would read better.
- [ ] Expose ragdoll, reach and attachment to reflection, the editor and JS.
- [ ] Cone particle emitters emit about world +Y whatever the emitter's
  rotation; an oriented cone needs the emitter rotation in the GPU record.

## Vehicle deformation qualification and extensions

The implemented `SurfaceDrive`, impact events, private geometry updates and
plastic cages are documented in SPEC section 5.1; R1World integration and its
contracts belong in the game's technical documentation.

- [ ] Qualify a real headset, both eyes, sustained CPU/GPU frame times and
  memory under multi-vehicle collisions. Shared mono/multiview geometry and an
  OpenXR native build do not prove headset performance.
- [ ] Qualify browser deformation on constrained GPUs/mobile hardware, beyond
  the local Windows Chrome runtime/restart proof. R1World's Web port remains
  separate work; its packs/geographic delivery are not solved by these changes.
- [ ] Extend the bounded lattice to optional mechanical zones and independent
  part segmentation. Group shape matching, tearing, glass breakage and complete
  shell/self-collision are not implemented by the current displacement cage.
- [ ] Evaluate atomic GPU publication, shared topology/binding preparation and
  fair scheduling under workloads beyond eight damaged cars. The current fixed
  topology uploads reuse their allocation and can expose intermediate slices.
- [ ] Qualify constraints attached to bodies whose convex hull is rebuilt;
  vehicle validation covers preserved velocities, not arbitrary joint chains.

## Rendering compatibility and environment lighting

- [ ] Firefox WebGPU: the same FirstMinutes3D Web export stays black in Firefox
  after `[WebCanvas] 'UI' CPU raster 1920x1080` (Chrome renders it). Capture the
  Firefox console to find the refused shader module or pipeline (Firefox's WGSL
  validation and limits differ from Dawn's), then fix it in the engine and add
  Firefox to the Web verification.

- [ ] Replace the roughness-selected ordinary equirectangular mip chain with
  a GGX-prefiltered specular environment. Keep offline/once-per-source work,
  bounded memory, and prove constant-radiance conservation, localized bright
  source response and native/Web parity before replacing the current path.

## 0. Status

The public beta cycle is open. `main` includes work after the immutable Beta 4
tag; advance the product version as part of the next release procedure.
Release qualification follows [RELEASE.md](RELEASE.md). Implemented contracts
belong in [SPEC.md](SPEC.md); completed changes remain in Git and CHANGELOG.

## 1. Before stable publication — Windows distribution and signing

- [ ] Produce the Windows installer **Authenticode-signed** with the publishing
  key, then inventory the SHA-256 of the signed bytes.

- Confirm publication from a clean release commit with the checks in RELEASE.md.

## 2. Structural code debt

Split mixed responsibilities only when work requires it, with explicit state
boundaries and unchanged contracts. Measure current sizes with tools/code_metrics.sh.

### 2.1 Remaining god classes

| File | Responsibilities to separate |
|---|---|
| `src/render/Renderer.cpp` | Frame orchestration, descriptors, pipelines, GI, shadows and XR; see section 3 |
| `src/scripting/JsEngineBindings.cpp` | Bindings by JS namespace |
| `src/editor/panels/InspectorPanel.cpp` | Inspector sections by node family |
| `src/scene/WebCanvasNode.cpp` | UI runtime, world panel, scripts and reload |
| `src/core/Input.cpp` | Input backends and profiles |

**Extraction constraints:**

- **No gratuitous class.** A new class exists only if it has real **state + an
  invariant**. Splitting a file without a state boundary is forbidden.
- One responsibility per file, file named after its class; the header states in
  one sentence the responsibility **and the invariant**.
- One commit = one extracted unit, **with no behavior change**. A contract
  change (format, JS API, snapshot) is a separate commit.
- Rebuild **native + authoring WASM + Web player** as soon as a reflected type
  or the registry is touched. The web targets have separate CMake source lists
  (`web/*/CMakeLists.txt`): any `git mv` must update **both**, otherwise the web
  build breaks without the native one noticing.
- Use `tools/code_metrics.sh` for current file and function sizes.

### 2.2 Long functions and source hygiene

Review long orchestration/binding functions when modifying their domain.
Keep LF line endings according to `.gitattributes`; avoid unrelated churn.

## 3. Renderer — deferred decomposition (blocking prerequisite)

Renderer extraction remains deferred until the affected paths have visual coverage.

### Target decomposition

| Unit (state/invariant) | Methods taken over |
|---|---|
| **PipelineCache** — pipelines & layouts; rebuilt on format/swapchain change | `createGlobalSetLayout`, `createPipeline`, `createWebCanvasWorldPipeline`, `createCullingPipeline`, `createTonemapPipeline`, `createXrPipelines` |
| **FrameDescriptors** — global descriptor sets + per-frame-in-flight UBO | `createUniformBuffers`, `createGlobalDescriptorSets`, `rebuildGlobalSet`, `updateGlobalShadowDescriptor`, `updateUniformBuffer` |
| **GIRenderer** — GI descriptors, preparation and update cadence | `prepareGI`, `shouldUpdateRealtimeGI`, `updateGIDescriptors`, `updateEnvironmentDescriptor` |
| **GpuDrivenCuller** — indirect draw buffers + culling pipeline (behind the flag) | `createGpuDrivenBuffers`, `uploadGpuDrivenDraws`, `featureDraws`, `buildFeatures`, `recordFeatures` |
| **XrRenderer** — XR targets/pipelines + multiview UBO | `createXrTargets`, `cleanupXrTargets`, `updateXrTonemapDescriptorSets`, `updateUniformBufferXr`, `recordXrScenePass`, `recordXrWorldWebCanvases` |
| **SceneGatherer** — draw-list + lighting UBO from the scene | `gatherScene` (split), `recordMeshDraws`, `recordWorldWebCanvases`, `recordShadowPasses` |
| **Renderer** (thin remainder) — frame orchestration | `setViewportRect`, `clearViewportRect`, `activeRenderRect`, `drawFrame`, `recordCommandBuffer` |

### Coverage constraints

1. **The rendering check is narrow.** The golden-image gate (§3.1) now catches
   a wrong descriptor, an inverted pass order, a changed shadow bias or a wrong
   blend — but only where the WitnessGame hub frame shows it. That frame
   exercises meshes, one light, shadows, tonemap and the HUD; it does **not**
   exercise GI, GPU-driven culling, XR, transparency or any material the scene
   does not use, and those paths still break silently. Extending the fixture set
   is the cheapest way to widen the net before touching a unit that the hub
   scene does not draw.
2. **Per-platform/feature `#ifdef` branches** (`SAIDA_RHI_WEBGPU`,
   `SAIDA_ENABLE_XR`). The native build exercises only one branch; the web build
   compiles the other but does not run it on this machine.
3. **Shared state.** Preserve GI, shadows, tonemap, culling and XR descriptor
   ownership and pass order during extraction.

4. **Very long functions** to split at the same time (§2.2).

### Extraction prerequisites

1. **Establish a visual verification net. Absolute prerequisite.** The harness
   exists and works — **as a local check.** `tools/witness_golden_image.sh`
   captures frame 30 of the WitnessGame hub scene (mesh, light, shadow, tonemap
   and HUD in one image) and compares it against a committed reference,
   exactly. Contract in SPEC §6.3. What is **not** achieved is running it in CI,
   which is what this item asked for.

   **Proven blocker: pixel equality is not reproducible across GitHub-hosted
   runners.** On one machine the capture is byte-identical run to run. Across
   the hosted runners it is not: with `mesa 26.1.7` and `llvm-libs 22.1.8`
   identical on all three, three CI runs produced **two** different frames,
   differing on 5 634 of 230 400 pixels at a channel delta of 1. Thread count
   (`LP_NUM_THREADS`) and vector width (`LP_NATIVE_VECTOR_WIDTH`) were each
   measured to change nothing, which leaves CPU-dependent JIT codegen in
   llvmpipe — and a hosted runner does not let us pin the CPU.

   The obvious escape is closed too: a tolerance of 1 absorbs that difference,
   and also absorbs a 1% change to the AO exponent (2 175 pixels at the same
   delta), so it reports PASS on a genuinely changed renderer. The gate cannot
   be both reliable and sensitive on this infrastructure, so it is not wired
   into CI — a permanently red check is one everybody learns to ignore.

   To close this item, one of: a runner with a pinned CPU (self-hosted, or a
   fixed instance type); a software rasterizer whose output does not depend on
   host codegen; or a verdict that is not pixel equality and is still sensitive
   below the noise. Until then a renderer change is gated by running the
   harness locally, which CONTRIBUTING requires.

   One deliberate narrowing of the original plan: the gate runs on **Lavapipe
   only**, not "on Lavapipe *and* a real GPU". Measured, the same build differs
   between llvmpipe and an Intel Iris Xe by 69 268 of 230 400 pixels — ordinary
   floating-point divergence spread over every lit surface. A tolerance wide
   enough to absorb it would hide any regression worth catching, so a real GPU
   is not gated. Per-GPU references would be gating the driver, not the engine.

   What this net does **not** do, and no pixel comparison can: say the frame is
   *right*. It proves the frame did not change. A re-recorded reference must be
   looked at before it is committed.
2. **Extract one unit at a time, leaf-first order**, one commit + one visual
   check per unit: ~~`TonemapPass`~~ **done** → `GpuDrivenCuller` (behind its
   flag) → `XrRenderer` (guarded) → `GIRenderer` →
   `PipelineCache`/`FrameDescriptors` → thin `Renderer` last.

   `TonemapPass` is already extracted. HDR targets and pass orchestration stay
   in Renderer; the unit receives input views and records into an active pass.
   XR owns separate layouts/samplers. Preserve that ownership boundary.

3. **Pass ownership: passes never call each other.** Only
   `Renderer::drawFrame` sequences the passes. Each unit owns its GPU objects
   (pipelines, descriptors, targets) with a create/destroy cycle tied to
   swapchain/format changes, and exposes only `record*`/`update*`. No unit reads
   another's descriptors.
4. **Platform boundaries: `#ifdef` variations stay INSIDE each unit.**
   `TonemapPass` handles Vulkan vs WebGPU internally; the frame orchestrator
   stays agnostic.
5. **Verify each unit**: native **+ web** build + Witness E2E + **visual diff**
   (point 1) + XR smoke if a headset is available.

## 4. Manual checks still useful

The automatic net covers: compilation (native + web), headless logic, gameplay,
HUD rendering, and — for the hub scene on Lavapipe only — that the rendered
frame has not changed. It **does not cover**: whether that frame is *correct*,
any rendering path the hub scene does not draw, real-GPU output, the editor GUI,
XR, or the semantics of MCP mutations. Changes to these areas need the corresponding manual or device verification.

- **Editor** — `witness_editor_play` and `witness_editor_build` cover the
  Play/build paths, but not the ImGui clicks in edit mode. Manually verify
  gizmos (T/R/S drag, click-selection, collider wireframe), panels, Settings
  tabs and the three project modals against the viewport during a future UI
  session.
- **MCP** — the catalog of 45 tools, uniqueness, error dispatch and a real TCP
  call are covered. Progressively add semantic tests per mutation handler as
  these tools evolve.

## 5. P1 — Subsystem quality

Post-V1 unless the scope changes explicitly.

- [ ] Network: complete P2P connectivity and qualify real-network sessions (SPEC 11.7).

  - [ ] Private addresses for players behind one router whose LAN does not
    carry multicast and whose router does not hairpin.
  - [ ] Qualify a session between two machines on different home networks,
    and on a mobile carrier (CGNAT). Any future JS exposure needs an explicit
    capability and permission-policy tests (SPEC 6.2); the Web runtime has no
    raw UDP.

- [ ] Rendering: optimize and refine the dynamic outdoor GI in SPEC 4.1.
  The Lavapipe presentation failure does not block this work; retain frozen
  references and strict diagnostic thresholds.

  - [ ] Reduce subvoxel leaks and temporal noise; improve probe placement and
    classification, transparency, adaptive hysteresis and firefly rejection.

  - [ ] Add a water voxel injector, textured terrain contribution, and optional
    per-node contribution controls. Profile spatial queries during heavy editing
    and streaming, and add adaptive budgets only where measurements justify them.
  - [ ] Extend sustained movement/forest/weather coverage across quality tiers,
    validate occluded reflections, and qualify the complete MX450 1080p frame
    budget separately from the bounded probe-update cost.
  Not Radiance Cascades: tried before this repository's first commit and it
  did not work (user, 2026-10-21); the reason was not recorded, so record it
  if they are ever reopened.

- [ ] XR: validate targeted headsets/runtimes, controllers and hand tracking.
  Default skinned hands now use the MIT Godot OpenXR demo GLBs; both handed
  rigs, world/mesh-space pose conversion, finger motion, invalid-joint fallback,
  tracking visibility and scene reload are covered by `saida_xr_hand_tests`
  without a headset. Visual alignment and latency still require hardware.
  Asymmetric eye depth reconstruction and per-eye mesh lighting are corrected;
  `saida_xr_projection_tests` covers surface/normal consistency under head
  motion. The reported lighting instability on Meta Link disappeared with SSAO
  alone disabled (VRDemo `DiagnoseLighting.ps1 -Mode NoAO`, 2026-10-21); SSAO
  has since been rewritten and now occludes indirect light only (SPEC 4.1,
  `saida_ssao_tests`). Binocular agreement and the instability with AO enabled
  still need a headset comparison against that NoAO baseline; CPU tests and
  desktop captures do not establish it.

- [ ] Qualify XRGesture on actual headset/runtime combinations: native extension
  availability (including PC Link), thumb contact/swipe accuracy and false
  activations, tracking/focus loss, left/right mapping, latency and R1World
  collision/streaming during head-relative stepping. Compilation and synthetic
  skeleton tests do not establish device recognition quality.
- [ ] XR: multiview MSAA/resolve, ImGui overlay and a real anchors backend.

- [ ] Physics: complete queries, constraints (slider, cone, motors) and
  diagnostics.
- [ ] Physics: prove the whole-subtree collider on real multi-mesh content.
  `CollisionShapeNode` now merges every mesh under a body (Mesh and ConvexHull
  alike, and Auto unions their bounds), replacing the single-mesh `findMesh`.
  What is missing is a check: `Mesh` needs a `GeometryRegistry`, hence a device,
  so there is no headless test of it — the guard is the Witness E2E and the
  golden gate, neither of which draws a multi-mesh body. Either give `Mesh` a
  CPU-only construction path for tests, or make `GTAClone`'s drive harness a
  runnable gate (see below).

- [ ] Tooling: GTAClone's `e2e_drive.js` does not pass under Lavapipe. Measured
  on 2026-08-20 against an unmodified `main`: throttle reaches 1.96 m/s, the car
  travels 0.00 m along its own forward and steering turns -0.0 deg. The city is
  568 road tiles on a software rasteriser, so the likely reading is frame budget
  rather than physics — `witness_e2e.sh` says outright that it wants a real GPU.
  Until someone runs it on one, the city's collision has no automated verdict,
  which is exactly the content the whole-subtree collider above exists for.

- [ ] Vehicle: nobody is visible behind the wheel. Seating a player disables
  their node, so a driven car is empty to look at and a passenger cannot be
  expressed at all. The honest fix is to parent the character to a seat node on
  the car and pose it, which needs two things the engine does not have: a
  reparenting call on the script surface, and a decision about what happens to a
  `CharacterBody`'s live Jolt body when its parent starts moving under it.
  Neither is cosmetic, which is why the car is emptied rather than half-filled.

- [ ] Rendering: a texture allocation failure crashes instead of degrading.
  Observed intermittently while launching the engine on GTAClone dozens of times
  back to back: `[error] failed to create texture image`, then a crash report.
  The same command passed on the two following attempts, so this is resource
  pressure across repeated processes rather than anything in the scene — the
  cause is not isolated and should not be guessed at. What is worth fixing
  regardless is the response: running out of a GPU resource is a condition the
  engine can be handed at any time, and it should name what it could not allocate
  and how much was asked for, then fail the load, rather than take the process
  with it.
  Native asynchronous TextureCache preparation/transfer now catches these
  failures and settles to its preallocated missing texture, preserving resident
  assets. The GPU regression rejects a valid decoded image whose extent exceeds
  the device limit; it does not force actual VRAM exhaustion. Legacy synchronous
  constructors and the Web path remain to qualify before closing this entry.

- [ ] Rendering: the Web backend's push-constant ring is too small for a city.
  Driving GTAClone in the Web player logs `rhi::webgpu: push ring exhausted
  (frame uses > 262144 bytes)` every frame — 2266 nodes overrun a 256 KiB ring.
  The scene still draws and the gameplay is unaffected, but the message is
  emitted per frame rather than once, which floods the console ring buffer and
  cost real time during this session: it pushed every `[E2E]` line out of reach
  and made a passing run unreadable. Fix by design: size the ring from the draw
  count rather than a constant, and report an overrun once per swapchain
  lifetime with the number that would have sufficed.

- [ ] GTAClone sample: nothing drives itself. All 30 cars are driveable and none is
  driven — the streets are still. The seam is already the right shape
  (`readsInput` off plus `vehicleDrive` through a `NodeRef`, which is what
  `scripts/driver.js` uses), so a traffic system is a script holding a car and a
  lane graph rather than a second vehicle implementation; `scenes/city.roadnet`
  already carries 568 lane nodes and 609 edges for it. Budget note: 30 dynamic
  vehicles cost less than the run-to-run variance of the frame time here, so what
  constrains traffic is triangles and bodies, not the vehicle solver.

- [ ] Animation: extended API (scrub, JS root motion) and BVH retargeting.

- [ ] Stabilize the GPU-driven flag and benchmark the classic path, bindless,
  indirect draw and compute culling on a reproducible corpus.

- [ ] Rendering: add a separate clearcoat lobe and evaluate bounded local
  reflections for smooth materials. The current PBR path reflects the HDR sky
  only (SPEC 4.1); automotive paint and nearby street reflections remain a
  quality gap. Choose the reflection technique after measuring its cost on
  the target hardware and respect the visual-safety prerequisite in §3.
- [ ] Rendering: point-light cubemap shadows and lightmap persistence if
  included in the product promise.
- [ ] MikkTSpace tangents (the normal map stays disabled without author
  tangents).
- [ ] Visual adaptation of prompts to physical Xbox/PlayStation controllers.
- [ ] Measure and optimize LTO only after stability.

## 6. P1 — Game authoring surface

The sections above track the engine's own subsystems. This one tracks the
surface a **game** touches — scene documents, scripts, HUD, input — which no
section owned so far. Most items were found by authoring a complete small game
against Beta 1 under the constraint of not modifying the engine: each one is
something that blocked, silently misled, or forced a workaround. §6.1 comes from
reviewing the scene-instancing path against the Godot model the engine follows.
None of them blocks a release; together they decide how much of a game can be
built without patching the engine.

### 6.1 Scene instancing — a scene instantiated inside another scene

**Vocabulary, decided: the reusable unit is a *scene*, not a "prefab".** A
`.scene` is the single composable unit, whether it is loaded as the main scene,
instantiated inside another scene, or spawned at runtime — the Godot model. The
engine must not grow a second asset type, a second file extension or a second
concept beside it, and the documentation, the editor labels and any new API say
"scene" and "scene instance". The existing `prefabAssetId` field and `[P]`
hierarchy icon predate this decision; the rename item below closes the gap.

The mechanism largely exists and is worth stating precisely, because the
remaining work is small and specific rather than a feature to build:

- **Authoring** — dragging a `.scene` from the content browser onto the
  hierarchy, either on the root area or onto a chosen node, creates the instance
  (`CreateNodeType::SceneInstance`): the file is loaded, the node is named
  `<stem> (Scene)` and the source is registered **by relative path** through
  `getOrRegister(path, AssetType::Scene)`. The instance shows as a collapsed
  leaf and double-clicking it opens the source scene.
- **Format** — `Scene::serialize` writes the source reference and *erases*
  `children`, so an instance stores no copy of its source. On load, the source
  file is re-read with `NodeIdPolicy::Regenerate` (no id collision between two
  instances) and the instance's own JSON is applied on top as root overrides.
- **Runtime** — `SceneTree::instantiate(path, parent)` instantiates by path with
  a JSON cache, ids regenerated; `SpawnerBehaviour` drives it on a timer, and an
  autoload whose value ends in `.scene` is instantiated at boot.

What is missing:

- [ ] Scripting: instantiate a scene from JS. `SceneTree::instantiate` exists and
  is used natively, but no binding reaches it, so `node.queueFree()` has no
  counterpart: a script can remove nodes and never create one. A game can place
  instances at authoring time and spawn them on a fixed timer through
  `SpawnerBehaviour`, but cannot instantiate on demand from gameplay logic — no
  projectile on fire, no wave on trigger, no pooled effect. Fix by design: bind
  `tree.instantiate(scenePath, parent?) → NodeRef|null`, **and** confine the path
  first: `SceneTree::resolvePath` returns absolute paths unchanged and performs
  no traversal check, so binding it as-is would hand scripts arbitrary `.scene`
  reads outside the project and break the SPEC section 6.2 confinement the module
  loader already enforces. Bound it (per-frame or per-scene cap) so a runaway
  script cannot exhaust memory.

- [ ] Scenes: per-node overrides inside an instance. `Scene::serialize` erases
  `children`, and the instance's JSON is applied to the **root** only, so an
  instantiated scene is all-or-nothing: no child's transform, material, behaviour
  property or enabled flag can differ between two instances of the same scene.
  Every variation therefore requires a separate `.scene` file. This is the
  largest gap against the Godot model the engine is following. Fix by design: a
  sparse override list on the instance keyed by a stable path or id **inside the
  source scene**, applied after the source is loaded; overrides for a node that
  no longer exists in the source must be reported, not dropped in silence.

- [ ] Scenes: reference the source by path, not by `AssetID`. An instance stores
  `prefabAssetId`, an `AssetID`, in a durable document — while the two other
  instancing paths (autoloads, `SceneTree::instantiate`) both use a
  project-relative path. `AssetID`s come from `generateID()` and the registry is
  regenerated whenever it is rejected or re-keyed (§5, formats item), so the
  reference can dangle; when it does, `getAbsolutePath` returns empty, the
  special case is skipped and the load produces an **empty `Scene` node** with no
  children and no diagnostic. The instance silently becomes nothing. Fix by
  design: store the project-relative path as the durable reference, resolve the
  `AssetID` from it, and fail loudly when the source scene cannot be resolved.

- [ ] Scenes: specify instancing in SPEC. Scene instancing appears nowhere in
  SPEC — not in section 3 (project, scene and authoring) nor in the
  format contracts — yet it is a durable document feature with an editor
  workflow, a serialization rule (children erased) and an id policy
  (`Regenerate`). Nothing states what an instance may override, what happens to a
  missing source, or whether instances may nest. Fix: specify it in SPEC section
  3 with the override and failure contracts above, and add a frozen corpus
  fixture covering an instance, an overridden instance and a dangling source.

- [ ] Scenes: rename the vocabulary to match the decision above.
  `Scene::prefabAssetId`/`setPrefabAssetId`, the `"prefabAssetId"` JSON key and
  the `[P]` hierarchy icon are the last places calling a scene a prefab;
  `CreateNodeType::SceneInstance` already uses the right word. The JSON key is
  durable content, so the rename is a format change and follows the repository's
  rule — producers, loaders, fixtures and tests move together, with a migration
  for existing documents rather than a silent reinterpretation.

- [ ] Scenes: remove the duplicated instance-loading block.
  `deserializeNode` handles an instance at creation time and returns early
  (`SceneSerializer.cpp:129`), then a second block near the end of the same
  function re-checks the reference and moves the source's children in
  (`:180`). The second copy can only run once the first has failed, and it
  repeats the very `getAbsolutePath` call that failed — so it never recovers
  anything and only duplicates the rule in two places. Fix: keep one path.

### 6.2 Remaining authoring friction

- [ ] Graphics: built-in primitives beyond the cube. `builtin:cube`
  (`kAssetBuiltinCube = 1`, `src/graphics/Primitives.hpp`) is the only geometry
  that exists without a file, while `CollisionShape` already offers `Sphere` and
  `Capsule` — a scene can collide with a sphere it cannot draw. Any round or flat
  object in a test scene therefore requires an imported glTF, pulling Git LFS and
  the asset pipeline into a five-node scene. Fix: sphere, plane, capsule and
  cylinder generated alongside the cube with reserved builtin ids, offered by the
  Inspector's mesh picker.

- [ ] UI: give `UITextNode` a text alignment. `HudRasterizer` emits one
  absolutely positioned `<div>` per node with no `text-align`, and `UINode`'s
  pivot offsets only the node's own rect (`getGlobalRect`), not the glyphs inside
  it. A centred score — the most common HUD element there is — cannot be authored
  without knowing the rendered width of the string. Fix: an alignment property
  mapped to `text-align` in the emitted markup, so pivot keeps meaning "where the
  box sits" and alignment means "where the text sits inside it".

- [ ] Scene loading: stop regenerating a duplicate id in silence.
  `ensureUniqueIds` replaces a duplicate or invalid `NodeId` rather than refusing
  the document, which contradicts the rule the repository imposes on itself —
  *no silent fallbacks for invalid durable content* — that the schema envelope
  honours a few lines away. It is not a straight refusal to write: the same
  function serves prefab instantiation under `NodeIdPolicy::Regenerate`, where
  fresh ids are the point, so the fix has to separate "these ids are meant to be
  new" from "this document contradicts itself" before it can refuse anything.

  The subtree half of this item is **closed**. It read as if a typo in a `"type"`
  silently deleted a node and its descendants; measured, every public entry point
  (`nodeFromJson`, `loadNodeFromSceneFile`, `loadIntoScene`) runs
  `validateTypeContract` over the *whole* tree before deserializing anything, so
  an unknown type at any depth is already refused with its path. What was missing
  was that `deserializeNode` itself relied on its callers having done that; it
  now refuses rather than skipping, and names the ancestry. Pinned by
  `saida_durable_load_tests`.

- [ ] Tooling: make a runtime `.scene` checkable headlessly.
  `saida_tool validate-scene` validates the **authoring snapshot** (string ids);
  handed a runtime scene (numeric ids, `schema`/`version` = `kSceneVersion`) it
  returns a complete and entirely credible list of errors. There is consequently
  no headless way to verify a scene at all: the only method is launching the GUI
  editor with `--play` and grepping the log for `loaded scene from`. Fix: detect
  the envelope and refuse the wrong document explicitly instead of validating it
  against the wrong schema, then add a real headless load verb for runtime
  scenes so CI and authors share one check.

- [ ] Scripting: make `tree.changeScene(path)` report the eventual load result.
  The binding currently returns `true` as soon as its argument converts to a
  string; it does not resolve the path or wait for the deferred load. A game
  therefore cannot distinguish a queued valid transition from a missing or
  rejected scene. If it consumes the exit item before the load fails, progression
  is stranded in the old scene. Return a transition handle or publish a
  completion/failure signal carrying the requested path and diagnostic. Cover a
  successful replacement, a missing file and a schema-rejected scene; queued
  acceptance alone must not be presented as load success.

- [ ] Input: allow a binding to be added, and validate the context.
  `Input::applyBindingProfile` assigns over `g_bindings` — it replaces the entire
  table — and `rebindKey` calls `unmapAction` before `bindKey`. A script can
  therefore only overwrite everything or destroy an action's existing bindings;
  there is no "add a key". A game profile ends up restating the engine defaults it
  does not use, purely to avoid losing them. Separately, a binding whose
  `context` is not exactly `"Global"` parses, registers and never matches, with
  no diagnostic: `parseInputBindingProfile` validates control names against a
  table but accepts any string as a context. Fix: an additive bind/unbind pair
  alongside `applyProfile`, and context validation at parse time.

- [ ] Scene authoring: let a camera be aimed instead of hand-rotated. `Camera`
  carries only a quaternion, so a fixed game camera is authored by computing
  `sin`/`cos` by hand into JSON, and a framing error is only discoverable by
  launching the editor and looking. Fix: an authoring-level look-at target
  (node or point) resolved into the transform at load, plus an editor action that
  writes the current viewport orientation into the selected camera.

- [ ] Formats: document the runtime `.scene` field reference. SPEC covers the
  scene model and the authoring snapshot, but the runtime document's per-node
  fields — which keys are optional, what the integers of `shapeType`,
  `lightType` and the particle `shape` mean, which mesh ids are built in — exist
  only in the hand-written `serialize`/`deserialize` pairs and in the example
  scenes. Authoring or reviewing a scene by hand means reading engine source.
  Fix: generate the reference from the reflection that already feeds
  `saida_tool describe-engine`, and extend `describe-engine` to cover the core
  nodes that are serialized by hand.

- [ ] Scripting: settle the `NodeId` convention on the JS side. `node.id` and
  `NodeRef.id` return a **BigInt** (`JS_NewBigUint64`), so an id throws under
  `JSON.stringify` and under arithmetic: it can be passed back to
  `tree.nodeById` and little else. SaidaOps already solved the same problem in
  the opposite direction — SPEC section 3 encodes `NodeId` as a 64-bit decimal
  string "so as to lose no bit in JavaScript". Fix: align the script surface on
  that same convention, or state the BigInt contract and its limits in SPEC
  section 6.3.

- [ ] Audio: decide whether a game can make a sound without shipping a file.
  `audio.play(alias)` is the whole API and aliases resolve to files declared in
  the `.saidaproj`, so a prototype cannot emit a single cue without authoring and
  committing an audio asset through Git LFS. This may well be the right
  constraint; it is recorded here so the answer is a decision rather than an
  omission. Either provide a minimal built-in cue or a procedural tone, or move
  this to §7 as an accepted constraint.

### 6.3 UI authoring — parity, traps and the feedback loop

Written after building a full main menu (3D logo, file-select panel, keyboard
and pointer input) in a real project against the engine as it stands. The menu
shipped, but almost every hour went into *seeing* and *diagnosing* rather than
authoring, and every defect met was silent: no error, no warning, only a wrong
picture. That asymmetry is the subject of this section. It matters beyond one
menu — an agent authoring UI has no eyes, so anything the engine does not
report simply does not exist to it.

Ordering: **6.3.1 before everything else** (nothing below can be verified
without it), then 6.3.2 (removes the trap classes at the root), then 6.3.3.

#### 6.3.1 The feedback loop — an agent cannot see what it builds

- [ ] MCP: no UI tools at all. The catalog is 45 tools over animation, assets,
  nodes, scenes and scenarios; an agent driving a live editor can move a node
  but cannot see or inspect a document, while `hotReload` on `WebCanvasNode`
  already re-reads a document from disk. The iteration loop that should exist —
  edit, reload, look, assert — is missing only its observation half. Fix:
  `ui_screenshot`, `ui_layout` (the JSON above, live) and `ui_reload` against
  the running editor.

- [ ] Tooling: drive a real `WebCanvasNode` by element id or selector. The
  current VerticalSlice E2E calls `tree.changeScene` directly, so it proved the
  gameplay scene while completely bypassing the menu button that launches it.
  Add a deterministic interaction harness available from `saida_tool`, the
  editor test CLI and MCP: `ui_focus`, `ui_pointer_move`, `ui_pointer_down`,
  `ui_pointer_up`, `ui_click`, `ui_key` and `ui_text`, all routed through
  `UIInteractionSystem` and the same RmlUi/QuickJS bridge used by a player.
  It must be possible to express "focus `#play-button`, activate it, wait for
  `scenes/verdance.scene`, assert `#hud`" without screen coordinates. A helper
  that invokes `tree.changeScene` is not equivalent proof.

- [ ] Tooling: capture animated UI as a deterministic sequence, not only one
  still frame. Add a fixed-step mode to `render-ui`/`ui_screenshot` with an
  event script and checkpoints, producing numbered PNG frames plus layout/state
  JSON at requested times. Record active pseudo-classes, classes, focus,
  computed opacity/transform and the running transitions/animations. A compact
  contact sheet or image diff across checkpoints should make hover, focus,
  entrance, reduced-motion and scene-exit animations reviewable by an agent
  without watching a live window.

- [ ] Tooling: expose an ordered UI event and mutation trace. For each driven
  interaction, report event type, target/currentTarget, listener entry/exit,
  focus owner, class/style/text mutations, deferred layout flushes, document
  generation and scene-change/destruction boundaries. Bound and filter the
  trace by canvas or selector so it stays usable. This would have shown the
  `focus -> classList.add -> Context::Update -> focus` recursion and the later
  call into a destroyed menu in one run instead of requiring Windows Event
  Viewer, GDB and manual RVA symbolication.

- [ ] Editor UI inspector: expose the complete pointer-routing decision for the
  current frame. Show window coordinates, the dockspace central rectangle,
  canvas-local coordinates, the RmlUi element under the pointer and its border
  box, the `WebCanvasNode::hitTest` result, whether the event was consumed, and
  which system owns cursor capture. The overlay and a machine-readable MCP
  result must use the same data. This incident otherwise presented three
  indistinguishable symptoms: a viewport-offset transform, a canvas that did
  not receive mouse moves, and a correct hover state whose colour was still
  transitioning.

- [ ] Tooling: symbolize local crash reports automatically. The crash reporter
  writes the build commit, exception RVA and minidump, but a development build
  still requires manually matching the image base and invoking GDB/addr2line.
  Add `saida_tool symbolize-crash <log-or-dmp> --symbols <dir>` (and make the
  editor offer it) to emit a source-level stack, identify a missing/mismatched
  symbol artifact explicitly and preserve the original immutable report.

#### 6.3.2 Silent traps — each one produced a wrong menu with no diagnostic

- [ ] UI runtime: make DOM mutation scheduling non-reentrant by contract.
  RmlUi dispatches focus and pointer listeners from `Context::Update`; a
  `classList`, style or text mutation inside one of those listeners must be
  queued and flushed once at the next safe UI boundary, never call `Update`
  recursively. Centralize the dispatch-depth/flush rule rather than relying on
  every binding to remember it, expose a diagnostic when a synchronous update
  is attempted during dispatch, and test focus, hover, click and animation
  callbacks that mutate the DOM.

- [ ] UI input: replace cross-frame raw interaction pointers with
  lifetime-checked handles. `UIInteractionSystem` keeps hovered, pressed,
  focused and touch targets while a deferred `changeScene` can destroy their
  entire `WebCanvasNode` after input handling. Use `NodeId` plus a document
  generation (or an equivalent invalidation token), clear every target
  atomically on hierarchy/document replacement, and cover mouse, keyboard,
  gamepad and touch. The corpus must change scene from `mousedown`, `click` and
  keyboard activation, then run at least one more frame; a direct
  `tree.changeScene` test does not exercise this lifetime boundary.

- [ ] UI input: define one screen-to-canvas transform and explicit cursor
  ownership for the editor. Rendering, RmlUi pointer movement, hit-testing,
  click dispatch and editor picking must all consume the dockspace central
  rectangle recorded by `EditorUI`; none may substitute the main OS viewport.
  Mouse movement must reach the RmlUi context whenever the pointer is inside
  the canvas so that enter/leave state remains current. The interactive
  `hitTest` result is only for click/scroll arbitration, and a click consumed by
  UI must never enable relative mouse capture or hide the cursor. Add a docked
  editor regression with asymmetric side panels and test button centres,
  borders, gaps, entry from both directions, clicking and leaving the canvas.

- [ ] UI tests: add a shipping-document interaction corpus. Load a real
  HTML/RCSS/JS `WebCanvasNode` without hand-reimplementing its behavior, drive
  its controls through the public interaction harness, advance a deterministic
  clock through its transitions, and assert pixels, layout, event order,
  document lifetime and the resulting engine action. Include the VerticalSlice
  `BEGIN THE JOURNEY` flow as a regression: no `Maximum call stack size
  exceeded`, no stale `ProcessMouseLeave`, the gameplay scene reaches ready and
  the old menu receives no later input.

- [ ] UI authoring: make pointer feedback latency visible and safe by default.
  The VerticalSlice buttons transitioned every hover colour for 180 ms. The
  hit target and RmlUi `:hover` state were correct, but while crossing a button
  the highlight became visible only near the opposite edge; reversing pointer
  direction reversed the apparent activation point. Ship menu templates whose
  primary hover affordance changes immediately, reserving transitions for
  secondary decoration or hover exit. Extend `validate-ui` to report controls
  for which every visible hover indicator is delayed, and have the interaction
  trace distinguish the pseudo-class timestamp from the computed visual
  transition. Do not change CSS transition semantics globally: diagnose the
  delay and provide a correct default.

- [ ] UI CSS-wide keywords: generalize computed `inherit`/`initial`/`unset`
  beyond the tested box-sizing contract, with shorthand expansion and dynamic
  invalidation; implement cascade-origin `revert`/`revert-layer` only with the
  corresponding real origin/layer semantics and browser comparisons.

#### 6.3.3 Content pipeline and authoring surface

- [ ] Make repeated native SDK packaging checks independent of an earlier
  staged package. A second full-suite run reached 101/102, with
  `saida_native_sdk_consumer` failing to overwrite its existing runtime
  (`File exists`). Removing only its generated `build/package` directory and
  rerunning that test passes. The initial clean full-suite run passed 102/102;
  no test or exporter was weakened to hide the repeat-run failure.

- [ ] UI: decide the Web parity of `WebCanvasNode`. The Web player compiles
  `UICanvasNode`, `UITextNode`, `UIRenderer`, `HudRasterizer` and the RmlUi CPU
  backend, but **not** `WebCanvasNode` (absent from `web/player` and
  `web/runtime` source lists). A project whose menu is a document therefore
  builds for desktop and silently loses its menu on Web — which contradicts the
  release criterion in §0 that the same game runs in the editor, on desktop and
  on Web. The port itself is bounded: the Web player already links QuickJS, and
  the HUD precedent shows the platform difference is confined to the texture
  upload (Vulkan bindless quad vs WebGPU texture + bind group). Fix: either
  port it — upload branch, source lists, input plumbing, one Witness scene
  proving the same document on both — or state in SPEC that documents are a
  desktop-only surface and that the portable UI is the node path. Do not leave
  it implicit.

- [ ] UI: demonstrate keyboard and gamepad navigation. RmlUi's `nav-*`
  properties exist and key events already reach the context
  (`WebCanvasNode::onKey` → `ProcessKeyDown`), so directional navigation is one
  stylesheet declaration away — but nothing in the engine, its tests or its
  samples shows it, so every project reimplements menu movement as a glue
  script binding keys by hand. Fix: `nav: auto` demonstrated in the templates
  above and covered by `saida_ui_interaction_tests`, so a menu gets D-pad
  movement without a script.

- [ ] Tooling: expose the model importer as a command. The editor has a 3D
  Importer; there is no headless equivalent, so preparing one mesh for a
  project means writing a conversion script against a third-party Python stack
  outside the engine. Fix: `saida_tool import-model <file> --out <glb>` over
  the same importer the editor uses, and a `import-sprite` sibling (crop,
  nearest-neighbour scale) for 2D UI art, which today means calling `ffmpeg` by
  hand.

## 7. P2 — Out of scope, kept as a decision

These items delay no release unless the promise changes explicitly:

- turnkey multiplayer replication/genre frameworks and large terrains; the
  generic UDP networking module was reopened by the user on 2026-10-23 and is
  tracked in §5;
- generalized SIMD animation, massive pose sharing and GPU crowds;
- research-grade GI beyond the optimized outdoor GI of §5 (reopened by the
  user on 2026-10-21); Radiance Cascades were tried before the first commit
  and did not work;
- advanced UI layers/filters and the WebGPU document backend; native document
  GPU rendering was reopened by the user's animated-UI requirement and delivered
  on 2026-10-20;
- KTX2/Basis (PNG/JPG textures everywhere today);
- full SaidaFX graph, trails/ribbons and advanced particle collisions;
- world model, skills and autonomous agents;
- asset store and online services, carried by the Saida platform;
- Linux and macOS editor/player, Firefox/Safari and mobile browsers.

## 8. Closed decisions — do not reopen without a new reason

- **Three `ReflectedTypes*` lists** (`.cpp`, `Player`, `Web`) kept distinct. The
  targets neither compile nor link the same subsystems (physics/Jolt, audio,
  QuickJS, scenario deliberately absent from some Web variants): a single source
  would force per-type `#ifdef` or unlinked symbols. `runtimeTypeMatrix`,
  verified at boot, remains the parity guardrail. The identical
  `registerBehaviour<T>`/`registerNode<T>` templates stay local: their small
  duplication is more readable than a cross-cutting header with no invariant.
- **MCP (`SAIDA_ENABLE_MCP`)** is an assumed V1 capability of the Saida
  workshop, ON by default, covered by a catalog test and a real TCP smoke test.
- **Python authoring remains an optional standalone plugin.**
  `plugins/python-tools` is not embedded in the engine, editor or exported
  runtime, does not participate in CMake and stores opt-in configuration outside
  `.saidaproj`. Its only native integration is an optional `saida_tool`
  subprocess adapter. An embedded interpreter, automatic editor execution or a
  mandatory Python asset pipeline would require a new architecture decision.
- **`EditorShell`** is not created: `EditorUI` is the final shell. A mirror
  class would have added neither state nor invariant.
- **`src/rhi/`** (383 lines) stays as is: it is the repository's cleanliness
  reference.
- **Assumed unmet metrics**: the V1 refactor aimed at 0 files > 800 lines and a
  largest file ≤ 600 lines; we stayed at 9 files > 800 lines with `Renderer.cpp`
  at the top. These goals were not "solved" by artificial compression or by
  extraction without an invariant — they remain open in §2.

## 9. Saida platform

The web platform, backend and operations live in
[`saias-o/saida`](https://github.com/saias-o/saida) and carry their own roadmap
as well as the global production go/no-go.
