# SaidaEngine — Roadmap

Updated: 2026-10-23. This file is the engine's **single backlog**: everything
that remains to be done, deferred or decided for later. It does not describe
what exists — the technical truth is in [SPEC.md](SPEC.md), and getting started
is in [README.md](README.md).

Rule: nothing is checked off here without the run, commit or exact corpus that
proves it. Closed work (V1 gates, V1 refactor) lives in the Git history and in
the corresponding contracts of `SPEC.md`.

## Contact shadows and camera coverage — 2026-10-23

- [x] Replace the angular depth bias and raster slope bias with per-texel
  receiver-plane PCF, and follow the camera with a texel-snapped directional
  volume. Proof: `tools/verify_shadows.py` passes ten GPU cases with GI/AO/IBL
  disabled (grazing and steep Sun, reversed direction, spot, 25/60/120 m
  extents, +/-150 m camera travel, softness 0/1/1.5), checking contact and an
  unchanged unoccluded floor that also casts shadows. Four of the original
  five cases failed before the correction. Native build and CTest 110/110;
  final sampler also checked by the terrain GPU, SSAO and streaming tests;
  AO pixel verifier passes; Web shaders transpile and Chrome accepts the
  scene pipeline. R1World rebuilt, two Paris views captured, and its actual
  skinned player captured on a self-shadowing floor with GI/AO/IBL off.

## Ambient occlusion on indirect light only — 2026-10-22

- [x] Occlude indirect light only, after a depth pre-pass, with a kernel turned
  per pixel and an edge-aware average (SPEC 4.1). Proof: `saida_ssao_tests`
  (both stages compiled from the shader files, within 0.001 of a 32-times
  denser kernel where a fixed one strays three times as far, and every earlier
  defect reinjected fails it), `tools/verify_ambient_occlusion.py` (direct
  light bit-identical with AO on and off on the GPU, GPU-driven path, MSAA),
  the other five pixel verifiers, `saida_terrain_rings_render_tests`, Vulkan
  validation with synchronization checks (nothing new with AO on, and the MSAA
  depth-resolve hazard it showed fixed), the Web player built and its new WGSL
  compiled and pipelined by Chrome WebGPU, and R1World's rue de Rivoli captured
  with AO on and off.
- [x] Bring the AO map's cost down: 2.3 ms instead of 4.3 in that view on the
  MX450 at 1600 x 900, the kernel and its denoise at half resolution with a
  depth-aware upsample (SPEC 4.1). Proof: `saida_ssao_tests` over all three
  stages, its bounds tightened (0.0005 mean, 0.02 worst, from 0.001 and 0.03)
  and still failed by every earlier defect reinjected, by a kernel the same for
  every texel, by the raw kernel undenoised and by an average off centre;
  `tools/verify_ambient_occlusion.py` and the five other pixel verifiers; Vulkan
  validation with synchronization checks, nothing new with AO on; the Web
  player built and the upsample's WGSL compiled and pipelined by Chrome
  WebGPU; R1World's rue de Rivoli profiled with AO on and off, the same draws
  as before.
- [ ] AO on still adds 3.5 ms to that view's GPU frame (19.1 to 22.5 ms), and
  the depth pre-pass is most of it: it costs 3.0 to 3.6 ms and saves 2.5 to
  3.8 ms of the opaque pass from one run to the next, so it does not always
  pay for itself. Measure where its time goes before changing it.
- [ ] XR: the AO path (multiview pre-pass, one AO layer per eye) is built and
  shares the desktop code, but no headset has run it yet.
- [x] Give bloom its own fullscreen vertex stage instead of `tonemap.vert`'s
  depth binding and 208-byte push block. Proof (GI phase-0 prerequisite,
  2026-10-22): native CTest 109/109; Chrome accepts both bloom pipelines with
  their real 32-byte layout and rejects the old vertex/layout combination
  (`tools/verify_postprocess_web.mjs`); Web player rebuilt. This removes the
  Lavapipe pipeline-creation crash reproduced with the original runtime.
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

## Terrain-hole raster cost — 2026-10-20

- [x] Reject whole cells inside convex terrain holes before rasterization,
  without changing visible shading. A compute invocation per cell produces
  per-frame-in-flight flags; the vertex shader emits degenerate covered cells.
  The perspective edge guard retains MSAA boundary samples in both views.
  Computing the guard six times per cell in the vertex shader was measured
  slower at eye height and was rejected. Terrains without holes skip the pass.
  Native CTest passes 105/105; thirteen image pairs compare exact pixels
  against fragment-only clipping, including water/rain, morphing, relief,
  grazing/subpixel cells and hole removal. On the MX450 at 1600x900, the fixed
  benchmark's total GPU time changes from 30.05 to 1.38 ms at 500 m and from
  23.01 to 16.53 ms at 1.8 m, including the culling pre-pass (0.10/0.28 ms).
  These fixture timings are not whole-game or all-world 30 fps qualification.

## Photographic sky / fog horizon — 2026-10-19

- [x] Match the lower HDR sky to the scene's fog colour after clouds and before
  the independent Sun, with a smooth 1–6 degree band around the local vertical.
  Preserve authored upper-sky pixels and fog-disabled scenes; hide a finite
  terrain's far clip in the last 10% of its range. This is a photographic
  horizon correction, not a new physical sky model.
  Proof: 148 GPU weather/horizon checks on the MX450, native CTest 100/100,
  mono/multiview SPIR-V and Web WGSL builds, both postprocess pipelines accepted
  by Chrome WebGPU, and fixed-camera/lighting before/after captures in R1World.

## Independent native projects — 2026-10-18

- [x] Export a source/build-tree CMake SDK, project-owned C++ registration,
  native player, optional project editor and matching native export. Proven by
  `saida_native_sdk_consumer`, a separate project outside the engine/game tree,
  MCP-generated C++ compilation, editor Play/Build and a packaged player with
  a Windows-only PATH. Development/export captures match exactly. R1World now
  consumes the same SDK and passes its complete offline Paris smoke.
  Native CTest: 100/100 after configuring with access to the submodule Git
  metadata; the former unknown-build-identity failure is resolved.
- [ ] Provide a relocatable installed native SDK with compiler/ABI identity.
- [ ] Qualify project-native C++ on Web; stock Web export currently rejects it.
- [ ] Support native-code reload without closing/rebuilding the project editor.

## Host memory admission — 2026-10-18

- [x] Expose renderer-independent Windows physical/commit headroom through
  `MemoryProfiler::availableHostBytes()`, with explicit unavailable results.
  R1World's producer admission/cache tests pass (3/3); an offline Paris walking
  smoke completes 24 standard/toaster switches without allocation errors,
  reporting roughly 3.0–3.2 GiB of private memory during the switches.

## Surface drive banking and HTTP audio — 2026-10-17

- [x] Add optional speed-dependent steering lock and supported-turn banking to
  `SurfaceDrive`, with zero preserving upright handling. Native vehicle impact
  regression passes; R1World's actual two-wheel colliders pass acceleration,
  both turn directions, braking, low-speed reverse and solver-child attachment
  at 30/60/120 Hz. The Web player and authoring WASM rebuild successfully.
- [x] Add asynchronous Windows HTTP audio through the OS MediaEngine, with
  explicit lifecycle/error states, bounded volume and safe callback ownership.
  Source/lifecycle tests pass; real Radio France MP3 playback reaches Playing.
- [x] Replace the Windows-only stream with the shared native libcurl/Mbed TLS
  and miniaudio backend, plus a WebAudio/HTML media backend. The portable media
  harness verifies native playback, real browser audio and an Android ARM64
  shared-library link. Full mobile/Quest exports and device qualification remain
  open; they are not implied by a successful media-library link.

## UI mouse gestures — regression, 2026-10-13

- [x] Preserve sampled mouse button history through UI consumption and stop
  synthesizing a second click after RmlUi's release. Verified on 2026-10-13 by
  `saida_input_consumption_tests` (left/right/middle holds and releases),
  `saida_ui_interaction_tests`, and R1World's real-menu zoom smoke (single and
  rapid clicks, long hold, zoom-out, bounds and reset). Native CTest is 96/97;
  the remaining failure is the previously documented unknown Git build identity
  in `saida_crash_reporter_tests`.

## Optional traffic add-on — control and spawn regressions, 2026-10-13

- [x] Anticipate host stop setbacks across short shape segments, complete
  committed signal crossings, and retire completed offscreen physical trips.
  Supply actual vehicle dimensions before spawn clearance. The standalone
  `plugins/traffic/tests/test_traffic.cpp` run passes on 2026-10-13, including
  a one-metre signal segment, red/green restart, phase-change clearance,
  physical terminal retirement, dense 12 m bus spawning, queue anticipation
  around bends, junction birth exclusion and stable model/size seeds. R1World's
  offline Paris 60-second check reports mean steady moving traffic rising from
  11.29 to 15.86, with zero junction/blocked-exit stops in the final sample.
  The final build passes a daytime check on 2026-10-14: mean 11.71 moving,
  zero incidents throughout and zero junction/blocked-exit stops at the end.

## Optional traffic add-on - region seam regression, 2026-10-14

- [x] Rank finite entry segments at adoption, resolve the continuation before
  the next physical command and preserve negative entry progress until a body
  crosses the seam. The standalone traffic suite passes, including early
  physical entry, consecutive short spans and two-region footprint following.
  Streaming hosts must retain forward ownership and expose transferred vehicle
  footprints to the old region's queue; the Paris integration is tracked in
  R1World's README: the final 120-second run at 48.857268, 2.352571 passes
  with no persistent physical stop within 60 m. A separate disabling incident
  remains beyond the next junction; this is not a claim of collision-free AI.

## Large streamed-scene frame cost — MX450 audit, 2026-10-12

- [x] Index enabled camera membership instead of scanning every scene node at
  selection time. Preserve hidden cameras, direct priority edits and hierarchy
  tie order after reparenting. Verified by the native scene/streaming CPU and
  GPU suite (301 checks) and optimized R1World Paris/Vannes traces: selection
  falls from roughly 15–16 ms to 0.01 ms. CTest is 94/95 in this sandbox; the
  remaining failure is an unknown configured Git build identity. Full local
  measurement conditions and limitations are in R1World's `game/README.md`.
- [x] Make transform propagation incremental while preserving retained mutable
  `Transform&` writes, parent changes, hidden nodes and physics results. Flat
  traversal records compare every local TRS but propagate only changed poses;
  topology changes reuse unchanged branches. The 2026-10-18 native streaming
  GPU suite passes 21,531 checks, including retained writes during unrelated
  topology edits, removals and sibling reordering. Before the final branch-reuse
  refinement, R1World's stationary Paris comparison reduced the two transform
  passes from roughly 32 ms to 6–8 ms. Sustained movement/streaming frame budgets
  remain to qualify in the game.
- [x] Reject rigid shadow casters outside each light's clip volume, including
  Vulkan near/far planes and conservative bounds under shear/negative scale.
  Skinned casters bypass this test. The same native suite covers clipping and
  offscreen camera casters that still intersect the light; Paris submits about
  900 shadow draws instead of 9,000 candidates without changing shadow settings.
- [ ] Isolate and reduce procedural terrain submission/sampling work without
  changing relief, surveyed tile holes or morphing/seam rules. The full grids
  still shade vertices for cells later discarded over resident tiles. Feature
  timestamp spans are about 18–19 ms in the local traces; confirm exclusive
  savings with A/B measurements because pipeline timestamp intervals overlap.

## Ragdolls, reach and attachment — 2026-10-13

- [x] Share player/crowd knockdown recovery, suspend animated colliders through
  ragdoll recovery and use the root physics world for mounted scenes. Verified
  2026-10-23: 93 ragdoll checks, native CTest 110/110, Web player build, offline
  Paris/Vannes nonlethal player hit and recovery; Vannes crowd fall and pistol.
- [x] Generic `Ragdoll`/`RagdollBehaviour`, `TwoBoneIKModifier`,
  `BoneAttachmentBehaviour`, `Animator::poseUpdated`, collision groups,
  `Behaviour::onRebase`/`onHierarchyInactive`, `Scene::ensurePhysics`,
  `AudioManager::preload`, and rigid meshes under an Animator no longer skinned.
  Verified: `saida_ragdoll_tests` (91 checks), native CTest 96/96 before the
  last renderer/audio edits (rerun pending), R1World offline smoke with a
  knock-down and a pistol round in Vannes.
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
evidence belong in the game's README.

- [x] Defer first-damage private mesh copies while assets are uploading or the
  geometry arena is full; retain source geometry and retry when capacity returns.
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

## Streaming and rendering integration — verified through 2026-10-09

- [x] Match mesh ambient fill in atlas impostors, keep hemisphere occlusion
  on diffuse environment SH only, and restore SH without texture fetches. Verified
  2026-10-12: 23 native billboard pixel cases (including sun-only versus
  occluded sun, ambient parity with meshes, blocked sky and red/blue crossfade), native
  classic/bindless shader builds, translated WGSL and Chrome WebGPU pipeline
  validation. Native CTest remains 94/95 with the same unknown configured Git
  build identity failure. Identical-light Near/Mid/Far captures and a local
  MX450 forest comparison are in R1World's
  `game/generated/tree-far-lighting-fix`. No authored assets changed.

- [x] Preserve per-pixel normals and static visibility on multi-view billboard
  atlases, with coverage-weighted blending and the authored instance frame.
  Verified 2026-10-11: 95/95 native CTest, 15 native billboard pixel cases
  (including no-tangent import, occlusion, instance yaw and transparent-view
  blending), mono/multiview SPIR-V and translated WGSL generation, and Chrome
  WebGPU scene pipeline validation without errors. R1World's Far keeps its
  unchanged albedo and four vertices; its bake and comparison evidence are in
  `game/generated/tree-contrast-validation`. Headset performance was not measured.

- [x] Add generic runtime rainfall and wet-surface shading, filtered world-space
  impact rings, opt-in material responses and procedural storm sky clouds.
  Share the equations across mono, multiview and translated Web shaders. Keep
  per-particle emitter forces and support exact runtime launch velocities for
  wind-driven rain. Verified 2026-10-09: optimized native build, 94/94 CTest,
  99 hidden Vulkan weather pixel checks with 18 captures and visual inspection,
  53 GPU
  particle state checks, and complete native/Web SPIR-V and WGSL generation.
  R1World's weather interpreter passes six CPU cases and the game compiles
  without launch. Evidence: `game/generated/rain-validation` in R1World.
  Headset output and reference-laptop performance were not measured; procedural
  storm clouds do not replace the HDR images used for environment reflections.

- [x] Stream child LOD representations using explicit local bounds and runtime
  readiness, preserving a loaded fallback and the outgoing fade when the
  finest-level limit changes. Accept immutable CPU triangle collision data
  independently of rendered geometry. Verified 2026-10-09: 92/92 native CTest,
  226 GPU streaming checks, CPU collision doorway/scale/replacement/rebase
  regressions, and R1World's offline Paris/Grenoble captures, Tunis interior
  traversal and Paris-to-Tunis walk/drive/teleport in
  `game/generated/building-lod-validation`. These are implementation checks
  on the current desktop, not a reference-laptop performance qualification.

- [x] Generic camera-facing multi-view glTF atlas materials with coarse live
  ambient/directional lighting. Verified 2026-10-09: native orientation, atlas
  blend and light intensity/color/elevation pixel contract, 92/92 native CTest
  (`tools/verify_billboard_materials.py`), 187 GPU streaming checks and translated
  WebGPU scene pipeline validation. R1World's 861-tree MX450 forest at 1280×720
  measures median GPU frame 22.98 → 5.52 ms with earlier Far selection; corpus
  `game/generated/streaming-validation/mx450/tree-far-study`. Complete Web player
  content execution was not repeated for this change.

- [x] Honor authored glTF dielectric specular/IOR factors and textures in
  streamed materials. Verified 2026-10-09: 92/92 native CTest; 182 GPU streaming
  checks including factor defaults, zero IOR/strength, distinct sRGB/linear
  texture publication and hidden ownership; native semantic pixel checks
  (`tools/verify_specular_materials.py`), environment/HDR and material-sidedness
  regressions pass. The translated Web scene pipeline compiles in Chrome
  WebGPU without validation errors (AMD RDNA 2); complete Web player content
  execution was not repeated for this change. R1World Near/Mid captures are in
  `game/generated/streaming-validation/mx450/tree-material-study`.

- [x] Decode streamed glTF models and embedded images on the AssetLoader worker,
  prepare procedural bounds/collision data off the main thread, and incrementally
  upload stable mesh proxies through a shared 512 KiB/frame transfer batch.
  Native transfers and complete mip chains now use polled fences; arena
  relocation retains its old buffers through an ordered copy without draining
  the device. Resource pumps yield at the pending-upload limit.
  Verified 2026-10-08: 92/92 native CTest, 119 GPU streaming checks (partial
  readiness, compaction, cancellation, staging ownership, mip readback,
  glTF hierarchy and shared consumers),
  native runtime contract and Web player build. R1World's local corpus is
  `game/generated/streaming-validation`: Paris/Grenoble captures and the
  Paris-to-Tunis walk/drive/teleport smoke pass. Environment-lighting/HDR and
  material-sidedness pixel checks pass. In the local Paris arrival capture,
  peak main-thread mesh upload time falls from 10.25 to 0.46 ms and texture
  finalization from 12.06 to 2.39 ms. The final Paris trace contains no CPU
  upload/queue waits during its frames; Grenoble retains one 0.40 ms legacy
  queue wait. Legacy synchronous registration and startup pipelines can still hitch;
  these captures do not demonstrate R1World's whole-world performance target.

- [x] Move native texture image/staging preparation, immutable RmlUi CPU raster
  snapshots and environment diffuse SH projection off the main thread. Bound
  texture preparations and row transfers; retain UI staging per fenced frame
  slot and retire canvases through ordered fences. Keep worker profiler scopes
  attached to their originating frame, even after history-slot reuse.
  Verified 2026-10-08: native and Web builds, 92 native CTest cases, 128 GPU
  streaming checks including partial texture readback and rejected allocation,
  118 UI corpus checks including byte-identical asynchronous snapshots, native
  exported UI pixels, and HDR/environment pixels in
  `build-rel/ibl-environment-run/run-mztyuow7`. R1World's local
  `game/generated/streaming-validation/fluidity-final-*` corpus verifies
  Paris/Grenoble arrivals and Paris-to-Tunis walking/driving/takeover/teleport.
  These local RTX 4070 captures do not qualify R1World's reference Ryzen 7
  5700U / MX450 target: sustained 30 fps at 1920x1080 everywhere in the world.

- [x] Encode display RGB once, load the neutral normal as linear data, and make
  diffuse/specular IBL and DDGI misses follow the visible sky pair, exposure and
  rotations. Verified 2026-10-07: 91/91 native CTest, HDR numerical levels and
  environment binding/crossfade/rotation/fallback pixels in
  `build-rel/ibl-environment-run/run-oios0s4m`, and sidedness pixels with explicit illumination
  in `build-rel/material-sidedness-run/run-a7m82mgg`. Native and WGSL shader
  variants compile. XR headset output and the Lavapipe golden gate were not
  exercised (no Lavapipe ICD installed on this host).

- [x] Restore Web player boot against the declared runtime matrix by omitting
  desktop-only Grass/TerrainRings registrations, and make single-mip GI atlas
  reads valid under non-uniform WGSL control flow. Verified 2026-10-07: Web player
  build and Chrome headless pixel capture in `build-rel/ibl-web-verified-run`.
  The player reports ready, with no browser or shader validation errors; its
  grey environment produces RGB 142 versus ACES/sRGB's expected 141.60.
  This is a rendering check, not a fresh Witness gameplay/release qualification.

- [ ] Replace the roughness-selected ordinary equirectangular mip chain with
  a GGX-prefiltered specular environment. Keep offline/once-per-source work,
  bounded memory, and prove constant-radiance conservation, localized bright
  source response and native/Web parity before replacing the current path.

- [x] Bind the selected sky texture when rebuilding the global IBL descriptor,
  instead of resetting it to the white fallback. Verified 2026-10-06: 91/91
  native CTest cases, `verify_ibl_environment.py` red/blue metallic pixel checks,
  material sidedness captures, and R1World's metallic car capture. Native GPU
  output was inspected; Web and headset output were not exercised by this fix.

- [x] Add physical-scale triplanar PBR layers to native terrain rings, filtered
  macro albedo/normals, and resource ownership for hidden terrain materials.
  Keep indirect probe lighting inside its measured volume. Verified 2026-10-05:
  90/90 CTest cases, terrain layer validation, 71 GPU streaming checks, and
  R1World's offline Grenoble/Cape Town/Rio captures. The layer API remains
  runtime-only; terrain rings still have no Web backend (SPEC section 2).

- [x] Construct mesh, hull and Auto geometry in the body's local frame without
  cancellation of large world translations. The GPU streaming regression builds
  a wall millions of metres away, rebases it, and checks front/back rays and
  thin sphere overlaps against its drawn surface (69 streaming checks).

- [x] Detect thin triangle walls from either side in scene rays and sphere
  overlaps. Covered by a mesh-wall front/back query regression, including the
  ignored-body filter, and R1World's character facade traversal checks.

- [x] Configure physics body, body-pair and contact capacity at startup
  (`PhysicsCapacity`, like `GeometryCapacity`). A body refused for capacity is
  logged once, counted, and no longer rebuilt and refused again every frame;
  step overflows are logged once per kind. Covered by
  `saida_physics_capacity_tests` and R1World's offline Paris runs, which logged
  about 2.2 million refusals before the change and none after it.

- [x] Resolve explicitly dimensioned boxes, spheres and capsules without
  traversing their body's meshes. The compound regression measures 0.049 ms
  for 1,024 boxes and 0.183 ms for 4,096 boxes; all 90 native CTest cases pass
  on 2026-10-05. Mesh-derived shapes retain their loading and bounds checks.

- [x] Release `CharacterVirtual` and its inner body when a character or its
  ancestor is disabled. Reconnect on reactivation; covered by the character
  disable/query/reactivate/move-and-slide regression and the R1World teleport
  between Paris and Tunis after driving and traffic takeover.

- [x] Replace global scene/resource rebuilds with per-scene branch revisions and
  incremental membership/resource ownership. Reuse the same ownership snapshot
  for trimming and the GPU budget; hidden and disabled branches retain assets.
- [x] Provide runtime subtree visibility, configurable geometry capacity, and
  child-group LOD selection using the existing projected coverage/hysteresis.
- [x] Add explicit world-frame rebasing for live bodies, characters and joint
  anchors. Run vehicle forces on actual fixed physics substeps.
- [x] Verify with 86 native CTest cases, `saida_scene_streaming_tests --gpu`
  (55 checks including real geometry capacity and multi-primitive LOD),
  `saida_vehicle_tests` (75 checks including 30/60/120/240 Hz equivalence),
  the native runtime contract, Witness export/run/restart and R1World's offline
  Paris-to-Tunis walk/drive/traffic-takeover/teleport test.

These changes do not replace mutable-transform traversal or add a script-level
reparenting/seat API. The vehicle seating item below remains open.

## 0. Status

The **V1** effort is complete on the engine side: gates P0.1 through P0.6 are
closed, the V1 refactor derived from the quality audit is closed
(ResourceManager 1102 → 414 lines, EditorUI 1933 → 384 lines, McpBridge
1401 → 77 lines, split of `src/scene/`, named Input/Renderer constants), and
the original V1 qualification passed: clean native build, 84/84 CTest, three Web builds
(player, runtime, authoring WASM), Witness E2E, `witness_editor_play`,
`witness_editor_build`, Web staging and a real MCP TCP smoke test (45 tools).

The public beta cycle is open. The remaining Windows distribution work before a
qualified stable publication is tracked in §1.

The latest native integration verification on 2026-10-07 passed 91/91 CTest
cases and the IBL/environment and material-sidedness pixel checks. This extends
the native record only; it is not a fresh Web, XR or release qualification.
`main` includes work after the immutable Beta 4 tag; the product version string
still reads Beta 4 and must be advanced as part of the next release procedure.

Reminder of the release criteria: the same WitnessGame must run in the editor,
standalone desktop and Web; old projects must migrate or be rejected without
corruption; memory must stay bounded; the published limitations must match the
observed behavior; the artifacts must come from a clean commit.

## 1. Before stable publication — Windows distribution and signing

- [x] Produce a portable Windows editor package. Beta 4 switches resource
  resolution through the adjacent `saida-install.json`, includes the complete
  runtime/resource/dependency closure and generates a deterministic ZIP plus a
  per-user installer from one staging tree. The development proof on 2026-08-25
  executed both formats outside the checkout: Hub/editor/CLI boot, WitnessGame
  export, run + persisted restart, exact install/uninstall all passed. Public
  qualification still requires the same script from a clean release commit.
- [x] Give the editor and Hub project paths that survive distribution. Both use
  the per-user application state, while creation and project discovery default
  to `%USERPROFILE%/Documents/SaidaEngine/Projects`; CI can isolate both through
  `SAIDA_STATE_DIR` and `SAIDA_PROJECTS_DIR`.

- [x] Add and verify Windows version resources on the four engine executables.
  `ProductName`, `FileVersion` and `ProductVersion` are generated by CMake from
  `EngineVersion.hpp`; the Beta 4 development proof read all four back as
  `SaidaEngine` / `1.0.0-beta.4`.

- [ ] Produce the Windows installer **Authenticode-signed** with the publishing
  key, then inventory the SHA-256 of the signed bytes.

The rest of the release chain is closed: mandatory CI (native build, 84 tests,
V1 corpus, deterministic fold, desktop and Web Witness); `saida_tool`, Web
player and authoring WASM published as pinned artifacts; byte-identical
Windows/Linux proof on the fold fixtures; byte-reproducible archives and NSIS
Witness installer **before signing**, recursive validation of DLL imports,
inventoried uninstall, documented immutable rollback; crash logs and symbols
tied to the version; SPDX SBOM + GPL/third-party notices + license/asset
inventory. Only trusted Authenticode signing and the clean-commit Beta 4
publication confirmation remain Windows distribution work.

## 2. Structural code debt

Finding of the quality audit: the codebase is healthy and disciplined (near-zero
TODO/FIXME debt, systematic fail-closed policy, frozen format corpus,
capabilities verified at startup). The points below break nothing; they will
cost more and more as the engine grows — the GPU-driven flag, XR and advanced
rendering are precisely blocked behind the first line of the table.

### 2.1 Remaining god classes

| File | Lines | Mixed domains |
|---|---|---|
| `src/render/Renderer.cpp` | 1894 | pipelines, descriptors, GI, tonemap, GPU-culling, shadows, XR, frame — see §3 |
| `src/scripting/JsEngineBindings.cpp` | 1853 | all JS globals (`node`/`time`/`input`/`tree`/`assets`/`audio`/`physics`/`storage`) → one module per namespace |
| `src/editor/panels/InspectorPanel.cpp` | 1336 | inspector for all node types → one section per node family |
| `src/scene/WebCanvasNode.cpp` | 1280 | a single node: RmlUi + world panel + scripts + hot-reload + placeholder |
| `src/core/Input.cpp` | 1050 | keyboard + mouse + gamepad + touch + web backend + profiles → one backend per device |

These splits violate a rule the repository already imposes on itself (README,
"Contribution rules"): *split omniscient classes and files that mix several
domains*. They remain out of scope as long as no work opens these files.

**Constraints to respect for any extraction** (derived from the V1 refactor,
they have proven themselves):

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
- `tools/code_metrics.sh` prints the numbers (files, LOC, files > 600 lines,
  long-function heuristic) to objectify before/after. Last measurement:
  **529 files / 74,864 LOC**.

### 2.2 Overly long functions

Beyond ~80 lines a function becomes hard to test and reason about. The longest
known ones are in `Renderer.cpp` and are handled with §3: `gatherScene`
(`src/render/Renderer.cpp:720`), `recordCommandBuffer` (`:1232`),
`drawFrame` (`:1403`), `rebuildGlobalSet` (`:499`).
`InspectorPanel` holds the maximum measured by the heuristic (483 lines).

### 2.3 Hygiene

- **Line endings**: several working-tree files are materialized as CRLF
  (`src/graphics/Mesh.{cpp,hpp}`, `VulkanDevice.hpp`, `hub/Hub.cpp`,
  `editor/BuildExporter.hpp`, …). `.gitattributes` (`* text=auto eol=lf`)
  normalizes them at commit time, but the inconsistency lingers on disk and
  pollutes diffs.
- **Near-zero TODO/FIXME debt** (2 `XXX`, 1 `BUG` across the whole tree): an
  achievement to preserve.

## 3. Renderer — deferred decomposition (blocking prerequisite)

`src/render/Renderer.cpp` (1894 lines) is the largest god class and the
highest-value decomposition: it unblocks XR, the GPU-driven flag and lightmaps
by making them testable in isolation. It is **deliberately deferred** — it was
taken out of the V1 refactor because it is not safe to do mechanically.

### Target decomposition (to execute *when the visual safety net exists*)

| Unit (state/invariant) | Methods taken over |
|---|---|
| **PipelineCache** — pipelines & layouts; rebuilt on format/swapchain change | `createGlobalSetLayout`, `createPipeline`, `createWebCanvasWorldPipeline`, `createCullingPipeline`, `createTonemapPipeline`, `createXrPipelines` |
| **FrameDescriptors** — global descriptor sets + per-frame-in-flight UBO | `createUniformBuffers`, `createGlobalDescriptorSets`, `rebuildGlobalSet`, `updateGlobalShadowDescriptor`, `updateUniformBuffer` |
| **GIRenderer** — GI descriptors, preparation and update cadence | `prepareGI`, `shouldUpdateRealtimeGI`, `updateGIDescriptors`, `updateEnvironmentDescriptor` |
| **TonemapPass** — ✅ *extracted* — tonemap pipeline/layout/samplers/set | `createTonemapPipeline`, `updateTonemapDescriptorSet`, `tonemapPushConstants` (now `TonemapPass::record`/`setInputs`/`pushConstants`) |
| **GpuDrivenCuller** — indirect draw buffers + culling pipeline (behind the flag) | `createGpuDrivenBuffers`, `uploadGpuDrivenDraws`, `featureDraws`, `buildFeatures`, `recordFeatures` |
| **XrRenderer** — XR targets/pipelines + multiview UBO | `createXrTargets`, `cleanupXrTargets`, `updateXrTonemapDescriptorSets`, `updateUniformBufferXr`, `recordXrScenePass`, `recordXrWorldWebCanvases` |
| **SceneGatherer** — draw-list + lighting UBO from the scene | `gatherScene` (split), `recordMeshDraws`, `recordWorldWebCanvases`, `recordShadowPasses` |
| **Renderer** (thin remainder) — frame orchestration | `setViewportRect`, `clearViewportRect`, `activeRenderRect`, `drawFrame`, `recordCommandBuffer` |

### Why it is risky (do not do it blindly)

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
3. **Deeply intertwined state.** GI, shadows, tonemap, GPU-culling and XR share
   the global descriptor sets and the frame orchestration
   (`drawFrame`/`recordCommandBuffer`). Cutting in the wrong place creates
   cross-dependencies between passes: **exactly the spaghetti we want to
   avoid.**
4. **Very long functions** to split at the same time (§2.2).

### What must be done FIRST

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

   `TonemapPass` (`src/render/TonemapPass.{hpp,cpp}`) owns the tonemap
   pipeline, its bind-group layout, its two samplers and its descriptor set,
   and exposes `setInputs` / `record` / a static `pushConstants`. Renderer went
   from 2 020 to 1 894 lines.

   Two deliberate deviations from the table above, both to keep the boundary
   clean rather than to save work:

   - **The HDR targets stay in Renderer.** `createHdrResources` allocates what
     the *scene pass* draws into, and `PostProcessor` samples it too. Moving it
     into the tonemap unit would make the scene pass ask the tonemap for its
     render target — a worse coupling than the one being removed. TonemapPass
     receives views through `setInputs` instead.
   - **`recordTonemapPass` stays in Renderer.** It also records bloom, the UI
     and the editor overlay into the same render pass, and rule #1 below says
     only `Renderer::drawFrame` sequences passes. The unit draws its fullscreen
     triangle into a pass the caller has already begun; it never opens one.

   It also removed shared mutable state that was not in the plan: the XR path
   was overwriting the desktop tonemap's layout and samplers. XR now owns
   `xrTonemapSetLayout_`/`xrTonemapSampler_`/`xrTonemapDepthSampler_`, and both
   paths build their constants through the same static `TonemapPass::
   pushConstants`, so one shader keeps one interpretation. Folding the XR
   tonemap into the unit is `XrRenderer`'s job.
3. **Anti-spaghetti rule #1: passes never call each other.** Only
   `Renderer::drawFrame` sequences the passes. Each unit owns its GPU objects
   (pipelines, descriptors, targets) with a create/destroy cycle tied to
   swapchain/format changes, and exposes only `record*`/`update*`. No unit reads
   another's descriptors.
4. **Anti-spaghetti rule #2: `#ifdef` variations stay INSIDE each unit.**
   `TonemapPass` handles Vulkan vs WebGPU internally; the frame orchestrator
   stays agnostic.
5. **Verify each unit**: native **+ web** build + Witness E2E + **visual diff**
   (point 1) + XR smoke if a headset is available.

## 4. Manual checks still useful

The automatic net covers: compilation (native + web), headless logic, gameplay,
HUD rendering, and — for the hub scene on Lavapipe only — that the rendered
frame has not changed. It **does not cover**: whether that frame is *correct*,
any rendering path the hub scene does not draw, real-GPU output, the editor GUI,
XR, or the semantics of MCP mutations. Any session touching these areas must be
supervised, not autonomous.

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

- [ ] Network: build a generic UDP networking module (requested 2026-10-23).
  - Provide non-blocking UDP sockets, bind/send/receive, IPv4/IPv6 endpoints
    and explicit lifecycle/error handling, with bounded queues and packet sizes.
    UDP is a datagram transport; peer connections are explicit sessions layered
    over it, with handshake, keepalive, timeout, disconnect and reconnect.
  - Expose reusable peer/session identifiers, packet sequencing, duplicate and
    stale-packet rejection, RTT/jitter/loss metrics, and optional acknowledged
    reliable delivery for control messages alongside unreliable gameplay data.
    Bound retransmissions and bandwidth; avoid fragmentation at the chosen MTU.
  - Keep the module independent of game topology, geographic tiles, discovery,
    entity ownership and replication rules. Allow games to build direct P2P
    sessions and exchange tick-stamped inputs/state for prediction and rollback.
  - Define transport validation and session authentication, reject malformed or
    oversized traffic, and bound per-peer resource use. Any future JS exposure
    needs an explicit capability and permission-policy tests (SPEC 6.2).
  - Qualify loopback, LAN and Internet peer sessions, packet loss, duplication,
    reordering, latency, jitter, timeouts and teardown without blocking frames.
    Report backend capabilities explicitly; native UDP must not imply raw UDP
    support in the Web runtime. Discovery and NAT traversal need their own
    proven solution before claiming arbitrary Internet P2P connectivity.

- [ ] Rendering: optimize and refine the dynamic outdoor GI now implemented in
  SPEC §4.1. On 2026-10-22 the user explicitly prioritized working, optimized GI
  over the original brief's mandatory proofs, phase order and extraction gates.
  The Lavapipe presentation failure is a backend issue, not a prerequisite for
  this work. Frozen native references and strict checks remain diagnostics.
  - [x] Fair CPU clipmap scheduler, toroidal retention, scrolling, bounded probe
    and voxel work, stable double-precision origin frame and transient warmup.
  - [x] Persistent atlases with partial blend/scatter, compact incremental voxel
    grids, cached mesh invalidation, cascaded tracing and continuous updates.
  - [x] Diffuse sky replacement, metallic/Fresnel response, material opt-out,
    voxel-traced hit lighting/solar visibility, emission and previous-probe bounce.
  - [x] Procedural voxel hook and desktop heightfield injection, vertex GI for
    cheap plants, water shading, quality controls and Follow integration in the
    outdoor client (including Toaster budgets and caller-managed origin rebases).
  - [x] Rays and solar visibility continue into coarser cascades instead of
    reaching the sky at a fine cascade's 16 m ceiling; voxel bricks come from
    one cascade per frame (one draw of the overlapping meshes, culled per
    brick); probes wait only for a cascade's first fill, not for an empty
    brick queue. Before, streaming kept the queues busy and Paris never
    passed 39 % warm-up; on 2026-10-22 the MX450 offline Paris smoke passes,
    warm-up reaches 100 %, `GPU/DDGI` 1.3 ms/frame mean, 3.0 ms p95
    (`--profile`, 600 frames, vsync off). `SAIDA_GI_DEBUG=irradiance|voxels`
    shows what surfaces receive.
  - [x] Back-face probe classification (> 25 % of rays) and full filtered
    sampling at every distance, replacing the nearest-probe path past 32 m
    that leaked sun onto shaded Paris facades and blackened others. One
    cascade per point plus the next across a half-cell band, tetrahedral
    taps without dynamic indexing. Fixed 1080p MX450 Paris view, 2026-10-22:
    `Scene/Opaque` 9.1 ms GI off, 15.1 ms before, 13.5 ms after; `GPU/DDGI`
    1.2 ms. The complete GI therefore costs about 5.5 ms there, above the
    brief's 2.5 ms; per-pixel sampling, not the probe update, is what remains
    (a constant-irradiance probe measured 9.4 ms).
  - [x] Half-resolution irradiance: a GI stage on the AO grid writes what the
    pre-pass surfaces receive, the AO upsample carries it into the map's
    colour (now RGBA16F, alpha the occlusion), and `shader.frag` /
    `terrain_rings.frag` read it instead of sampling probes per fragment.
    Same 1080p MX450 view: `Scene/Opaque` 9.6 ms (9.1 GI off), the stage
    0.5 ms, the whole GI 2.0 ms of `GPU/Frame` against GI off -- within the
    brief's 2.5 ms. Not yet run under Vulkan validation (layers absent on the
    reference machine) nor in Chrome WebGPU.
  - [ ] Reduce subvoxel leaks and temporal noise; improve probe placement and
    classification, transparency, adaptive hysteresis and firefly rejection.
  - [x] Stabilize stationary probe sampling with a fixed spatial scramble per
    persistent atlas slot, retaining realtime light/geometry updates and the
    existing budgets. On 2026-10-23 the MX450 same-session stationary corpus
    changes from peak display deltas of 10 (Anchored) / 6 (Follow) to 0 / 1;
    switching the Sun off still darkens the indirect receiver. The new
    `stability` case fails on the original renderer in both volume modes.
    The motion diagnostic now also captures one running world: the original
    fails at 6 m/s (33% adjacent jump), the fixed quadrature passes walking
    but still fails at 24 m/s (17% jump). The pre-existing open-sky energy
    diagnostic also remains outside its 2% bound; no thresholds were relaxed.
  - [x] Anticipate Follow coverage along the camera's smoothed velocity,
    prioritize entering voxel slabs and untraced probes ahead of maintenance,
    and blend cascades across two cells without increasing grid resolution or
    work budgets. Proof, 2026-10-23: CPU tests at 30/60/120 Hz, stops, reversals,
    teleports and continuously invalidated queues (the old scheduler fails the
    entering-brick priority check); `verify_global_illumination.py --cases motion`
    moves only the camera through static geometry at 6/24 m/s. At 24 m/s the
    worst sampled difference from settled lighting falls from 34% to 11%; the
    final walking case stays within 15%. Native CTest 110/110, WebShaders and
    Web player built, Chrome accepts the scene pipeline, and R1World's offline
    Paris warm-walking smoke passes. The strict `off`, `open` and `scroll`
    diagnostics fail both before and after; their references/thresholds were
    retained. Temporal noise and sustained all-tier movement remain open.
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
- [x] Add a generic pre-instance XR input-provider seam and optional XRGesture
  add-on: typed actions, merged profile suggestions, dependency negotiation,
  focus/reset semantics and native resource ownership; no Meta semantics in
  core input types. Synthetic gesture/sequence/locomotion tests and the real
  instance/action bridge against a fake OpenXR dispatch cover both native
  completion paths and extension rejection/fallback without opening a window.
- [x] Align native completions with Meta's OpenXR sample (changed-and-true,
  no local cooldown/neutral latch) and add explicit locomotion gating with
  tracking suspension/recovery, handoff and stop reasons. Quest 3/Oculus Link
  logs on 2026-10-26 showed native swipes continuing after the old gate closed;
  four-minute synthetic replay and regression tests cover the corrected path.
  Rechecking the changed behavior on the headset remains open below.
- [ ] Qualify XRGesture on actual headset/runtime combinations: native extension
  availability (including PC Link), thumb contact/swipe accuracy and false
  activations, tracking/focus loss, left/right mapping, latency and R1World
  collision/streaming during head-relative stepping. Compilation and synthetic
  skeleton tests do not establish device recognition quality.
- [ ] XR: multiview MSAA/resolve, ImGui overlay and a real anchors backend.

- [ ] Physics: complete queries, constraints (slider, cone, motors) and
  diagnostics.
- A scene's `skyboxTexture` is no longer exposed to a registry rescan: both
  scene readers accept a project-relative path, which the registry re-resolves
  afterwards (SPEC 3.1).

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
- [x] Honor material sidedness in classic, bindless and XR scene draws; reverse
  back-face shading normals. The instance ABI retains its existing size.
- [ ] Stabilize the GPU-driven flag and benchmark the classic path, bindless,
  indirect draw and compute culling on a reproducible corpus.
- [x] R1World streaming: queue lettering, interiors and distant ice, publish
  interiors/colliders only when ready, build native graphics pipelines in
  joined two-lane batches and reuse bloom pipelines on resize. The optimized
  Paris, Grenoble and Paris–Tunis traces have no legacy upload waits or pipeline
  creation inside frames; GPU scene streaming passes 153 checks.
- [x] Avoid general 4x4 multiplication when constructing affine TRS and compose
  camera transforms only along the selected camera's ancestor chain. Preserve
  hierarchy-order priority ties, hidden cameras, disabled branches and direct
  transform/priority edits. Verified 2026-10-08 on MX450: 92/92 native CTest and
  166 scene streaming GPU checks, including the added camera/TRS regressions.
  An isolated R1World Paris arrival pair at 1080p measures normal frame mean
  45.83 -> 39.05 ms and camera selection 13.18 -> 9.89 ms; it does not qualify
  sustained 30 fps worldwide on the reference Ryzen 7 5700U / MX450.
- [x] Grass fields accept shared north/south boundary knots; blade height and
  slope follow the refined triangle fans. Validate regular vertices, ordering,
  finite heights and bounded sample count; upload on field revision only.
  The 78 grass checks and native suite passed during this pass.
- [x] Add explicit-destination subtree rebasing to avoid cancellation when a
  distant frame moves nearby. Static and kinematic bodies synchronize to the
  recomposed tree; body identities survive. The final native scene streaming
  test passes 78 checks, including a distant-frame collision regression and
  direct parent edits. The GPU rerun is still pending.
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

- [x] Scripting: reach a reflected property from a script. `node.getProperty`/
  `node.setProperty`, and the same pair on `NodeRef`, bind the reflection the
  Inspector, SaidaOps, the MCP node tools and `.sseq` property tracks already
  write through (SPEC 6.3). Scripts were the fifth consumer of that reflection
  and the only one with a hand-written list of what it may touch, so a light's
  colour or intensity, a camera's field of view, a water's wave height or a
  particle's spawn rate were authorable and animatable but could not be changed
  by gameplay — SPEC 7 documents a `.sseq` animating "Sun intensity", which the
  engine could do on a timeline and a game could not do on a switch.

  Three things the implementation is deliberate about. Resolution is the node's
  own type then its behaviours, the order the sequence binder already uses.
  The value is kind-checked before it is applied, through `valueMatchesKind`
  promoted out of `SaidaOpApplier.cpp` into `reflect::` rather than copied — the
  reflected setters silently ignore a value of the wrong shape, so an unchecked
  write reports success on a property it never changed. And a failed write is
  logged while an absent property reads `null` in silence: probing is legitimate,
  losing a write is not.

  It also fixed a defect in the bridge it needed: `jsToJson` turned every JS
  number into a double, while the array/object branch went through
  `JSON.stringify` and produced integers. The two halves disagreed, so `[1,2,3]`
  reached a `vec3` and a bare `2` could not reach an `int` or an `enum` at all.
  Integral numbers now cross as integers, which is what `JSON.stringify` decides
  and what the object branch already inherited.

  Proof: `saida_js_property_tests` (headless, no device) covers both resolution
  halves, every kind a registered type carries through the live binding, all
  eight through the validator — no registered type declares a `quat` today —
  refusal of a wrong shape *with the value left unchanged*, an out-of-range
  enum, an unknown name and missing arguments. CTest 85/85, native build,
  `describe-engine`, `[CONTRACT] PASS native nodes=29 behaviours=21
  properties=217`, Web player and authoring WASM rebuilt, `witness_e2e.sh` and
  `witness_editor_play.sh` PASS. `NodeRef` parity cannot be proven headlessly —
  `SceneTree` needs a `ResourceManager`, hence a device — and was proven in a
  real scene through `--test-autoload`: read, write, wrong-kind refusal, unknown
  name and an enum write, all through a `NodeRef`.

- [x] Scripting: reach the scene's environment from a script. `scene.getSetting`/
  `scene.setSetting` read and write ambient, clear colour, fog, sky exposure, the
  IBL intensities, AO and bloom (SPEC 6.3). Reflected node properties had just
  made a light's colour and intensity scriptable, which left a day/night cycle
  half-connected: a game could redden and dim the Sun while the ambient it sits
  in, and the fog the distance fades to, stayed frozen at whatever the scene was
  authored with. Those are `SceneSettings` fields rather than node properties,
  and no binding reached them.

  The environment was also the last authored surface in the engine still
  described by hand at every consumer — the Inspector, the `set_scene_setting`
  op, the MCP tool and the serializer each with its own list of keys, its own
  idea of which ones existed, and for two of them its own spelling. Adding a
  fifth list for scripts is the mistake the reflected-property binding had just
  removed for nodes, so the environment is now described once, in
  `SceneSettings::describe`, and the two writers that resolve a field by name —
  the op applier and the script binding — go through it and nothing else. The op
  applier lost its hand-written key table in the process and gained the settings
  it never covered (`lightingMode`, `giMode`, `changeRenderingAtLoad`).

  Two consumers deliberately still do not. The Inspector's list is a *layout* —
  sliders, colour wheels, sections greyed out by bake state — and generating it
  from the reflection's range/tooltip/group metadata is its own change. The
  serializer's is a durable format: it resolves a skybox by path because ids are
  regenerated on rescan, and two of its keys have been written into every scene
  ever saved under names the ops spell differently. So the two descriptions are
  held to each other instead of merged: `saida_scene_settings_tests` requires
  every reflected setting to survive a save/load round trip and every saved key
  to stay reachable, with the exceptions — the skybox reference, the GI bake
  handshake, the editor's debug toggles — named in one place and justified
  there. A field added to one and forgotten in the other now fails the suite
  instead of becoming a setting that cannot be saved, or one no tool can edit.

  Two smaller things came with it. `TypeBuilder::colorProperty` describes a
  colour held as a `vec4` but authored as three channels with the alpha
  preserved, which is what the scene format, the ops and the inspector all
  already do and what a plain `property()` would have exposed to exactly one of
  them. Realtime GI samples current light directions on its update cadence,
  including directions written by scripts.

  Proof: `saida_scene_settings_tests` extended with the reflection/format
  equivalence in both directions, colour alpha, enum range checking and the
  absence of internal state; `saida_js_permission_policy_tests` updated for the
  new capability — it caught the addition on its own, which is what it is for.
  CTest 85/85, native build, `describe-engine` (the vocabulary is published
  under `sceneSettings`), `[CONTRACT] PASS native nodes=29 behaviours=21
  properties=217`, the three Web builds, `witness_e2e.sh` (3/3),
  `witness_editor_play.sh` and `witness_editor_build.sh` PASS. The binding
  itself cannot be proven headlessly — it needs a mounted `SceneTree`, hence a
  device — and was proven in a real scene through `--test-autoload`: a driver
  wrote a sentinel colour into the ambient and required the running cycle to
  take it back, then followed a computed dusk down and checked that the ambient
  darkened, the horizon warmed and the sky exposure fell together with the beam.

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

- [x] Tooling: render a UI document headlessly. `saida_tool render-ui
  <document> --project <dir> [--size WxH] --out <png>` drives the same CPU
  RmlUi backend `saida_ui_corpus_tests` exercises (`src/cli/RenderUiCommand.cpp`,
  SPEC 8.3). PNGs come from `core/PngWriter`, an in-tree fixed-Huffman deflate
  encoder with no third-party dependency, proven by decoding its output back
  with `stb_image` (`saida_png_writer_tests`, 38 checks: noise, alpha gradient,
  degenerate shapes, determinism, refusals). Proof: the VerticalSlice main menu
  renders at 1280x720 with zero diagnostics; `saida_tool_render_ui_valid`,
  `saida_tool_render_ui_rejects_malformed` and
  `saida_tool_render_ui_refuses_traversal` in CTest (76/76).

- [x] Tooling: dump the computed layout, not just pixels. `--layout-json
  <file|->` emits per element the tag, id, classes, computed `display` and
  border box, plus every diagnostic RmlUi raised. Engine log output is routed
  to stderr for the duration of the render so stdout stays parseable. It found
  the §6.3.2 trap on first use: a `<div>` declaring `width`/`height`/
  `text-align` reports `display=inline` with a `0x0` box — and `body` itself
  computes to `inline`, which is what the `engine.rcss` item below fixes.
  RCSS's silent property drops (`rgba()` with a 0-1 alpha) produce no
  diagnostic at all, confirming that item stays open for `validate-ui`.

- [x] Tooling: capture a frame, reproducibly, and give it a verdict.
  `--screenshot <png>` on the editor and the exported player writes the
  presented composite; `saida_tool compare-png` says whether two such images
  differ, by how much and where. Contract in SPEC §6.3.

  What made it usable as a golden image rather than a photograph: the capture
  waits for assets to settle before it counts frames, pins the frame clock, and
  is taken from the player (no live FPS/camera overlay). `CaptureScheduler`
  holds that policy away from Vulkan, so `saida_capture_scheduler_tests` proves
  it without a device.

  Proof: two independent runs of the exported WitnessGame at `--after-frames 30`
  are byte-identical (`MATCH 0/230400 pixels, max channel delta 0`); frame 1 vs
  frame 10 reports 131 277 pixels in a 640x231 box and writes its diff image; a
  size mismatch exits 1 before any pixel is examined. Native build clean,
  `ctest` 82/82.

  Capture resolution follows the window, so a committed reference is taken
  headless (`SAIDA_WINDOW_HIDDEN=1`).

- [x] Tooling: aim the capture. `--camera-pos x,y,z --camera-look x,y,z` parks
  the camera anywhere for the run, overriding the scene's cameras. Agents read
  images directly, so the useful thing to build was not a measurement layer over
  what looking already gives — it was the ability to choose where to look from.
  The defects that pass a structural check (geometry not meeting the ground,
  wrong scale, a floating surface) are invisible from the gameplay camera and
  obvious from a grazing angle; until now reaching such a viewpoint meant
  writing a JS harness. Refusals are covered by `saida_capture_args_tests`, and
  a rejected viewpoint exits non-zero before the loop rather than leaving a
  plausible PNG behind.

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

- [x] UI: ship an engine user-agent stylesheet. The baseline is embedded in
  `RmlUiRuntime` (`userAgentStyleSheet()`) rather than shipped as a file, so it
  cannot go missing from a package, and is merged *under* every document loaded
  through `RmlUiRuntime::loadDocument`/`loadDocumentFromMemory` — the three
  engine load sites (`WebCanvasNode`, `HudRasterizer`, `render-ui`) go through
  it. Specified in SPEC 8.2. Proof: `saida_ui_corpus_tests` (105 checks) pins
  both halves — the baseline paints the declared box, the same markup loaded
  bare still computes to `inline`, and a document's own `display` still wins.
  Measured on the trap document: `div` declaring `width`/`height`/`text-align`
  went from `display=inline` box `0x0` to `display=block` box `200x80`, with
  the text correctly centred.

  Extended on 2026-10-20 to semantic HTML containers, form controls and
  `[hidden]`, with a rendered authoring test. Tags outside the published list
  keep RCSS's `inline` default.

- [x] UI: give a screen-space canvas a reference resolution.
  `WebCanvasNode` carries `referenceWidth`/`referenceHeight` and a `scaleMode`
  (`Stretch` = the historic resize-to-window and still the default, `Fit` =
  the reference letterboxed inside the window, `Expand` = the reference kept on
  the constrained axis and the other one gaining logical room). The policy lives
  on the node (`applyViewportLayout`), not in the renderer, because rendering
  and pointer input both read the placement through `screenPosition`/
  `screenSize` — a rig that drew letterboxed while hit-testing full-screen would
  put every click in the wrong place. Whether a canvas wants the viewport at all
  is asked of its authored transform (`fillsViewport`), so a letterboxed canvas
  keeps being told about the window instead of freezing at its first size.
  Proved by `saida_webcanvas_scale_tests` (33 checks, no device): the three
  modes, a missing reference falling back to Stretch, and a click on a
  letterbox bar missing the canvas while the image's top-left maps to the
  document origin.

- [x] UI: wrap texture coordinates in the CPU RmlUi backend. The sampler wraps
  when the geometry asks for it, so `decorator: image(x.png repeat)` tiles and
  `tiled-box`/`ninepatch` rest on a mechanism that works. Whether to wrap is
  decided once per compiled geometry, from whether any of its texture
  coordinates leaves [0,1]: the same test applied per sample cannot tell a
  glyph's right edge at exactly 1.0 from the start of a second tile, and
  wrapping that edge to 0 tears every piece of text on screen. Clamped
  geometry keeps addressing texel centres exactly as before; wrapped geometry
  addresses whole texels and takes its neighbour round the edge, so the seam
  between two tiles filters like the inside of one. Proved by
  `testRepeatDecoratorTiles` in `saida_ui_corpus_tests`, which fails without
  the change with the row it renders in the diagnostic — one tile, then its
  last texel smeared to the far edge.

- [x] Tooling: lint a UI document. `saida_tool validate-ui <document> --project
  <dir>` reports four kinds, each of them silent today: `rejected-declaration`,
  `unresolved-asset`, `inline-box-properties` and `fractional-rgba-alpha`
  (specified in SPEC 8.3). The `rgba()` trap needed a text pass over the
  document and its stylesheets — confirmed during this work that RCSS drops the
  declaration with **no diagnostic whatsoever**, so there is nothing in the live
  document left to inspect; it is reported with `file:line` and the 0-255 value
  that was meant.

  Two deliberate precautions against false positives, because a lint nobody
  trusts is a lint nobody runs: `text-align` is reported only when it differs
  from the parent's computed value (an inherited alignment is doing its job),
  and RmlUi's anonymous `#text` elements are skipped (always inline, never
  authored). The `lint-clean` fixture exists to pin this: it contains a valid
  0-255 `rgba()` and an inline element with no box property, and must stay
  clean.

  Proof: `saida_tool_validate_ui_clean` and
  `saida_tool_validate_ui_reports_silent_traps` in CTest (78/78), the latter
  asserted on its output rather than only its exit code. The shipped
  VerticalSlice main menu lints clean. Engine-side, missing-asset warnings now
  travel the same diagnostic channel as RmlUi's own
  (`RmlUiRuntime::reportDiagnostic`) instead of going straight to the log, so a
  caller capturing diagnostics sees all of them.

#### 6.3.3 Content pipeline and authoring surface

- [x] Animated UI authoring baseline (2026-10-20): unscaled animation clock,
  requested-update deadlines, hover invalidation without clicks, top-left CPU
  alpha coverage, HTML-root/stylesheet/void-tag adaptation, Web alpha and
  millisecond/ease aliases, attribute/value/hidden DOM methods, textContent
  semantics and common style accessors. `render-ui --time` samples the CSS
  timeline at 60 Hz; unsupported shader decorators/filters are diagnostics.
- [x] Native document GPU baseline: immutable draw-list capture, cached source
  textures, premultiplied triangle blending and sRGB resolve, per-frame owned
  uploads/descriptors, fence-safe resize/retirement. CPU tooling and the
  `SAIDA_UI_RENDERER=cpu` comparison path remain available. GPU tests cover
  pixels, clipping, transforms, JS controls, idle animation, hover and resize;
  full WebGPU document parity and advanced layers/filters remain open.

  Verification on 2026-10-20: the RelWithDebInfo native build and 102/102 CTest
  cases pass, as do `describe-engine`, the native runtime contract, the Web
  player build and the authoring-WASM build/Node smoke. R1World builds against
  the SDK and its real-menu zoom E2E passes with the GPU canvas. All UI examples
  validate without diagnostics; captures at 0.5 s and 1.25 s differ as expected.
  The 1280x800 menu microbenchmark on the local RTX 4070 (16 warm samples)
  measured CPU raster median 21.01 ms and GPU submit-plus-fence median 0.141 ms
  (p95 1.23 ms). This excludes layout and is not MX450/whole-game qualification.
  The Lavapipe golden-image gate was not run: its ICD is unavailable in the
  configured UCRT64 environment. No frozen reference was regenerated.

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

- [x] UI: templates for the surfaces every game needs. Authoring the menu meant
  rediscovering, by reading engine and RmlUi sources, how a panel, a button, a
  focus state and a keyboard-driven selection are expressed here. None of it is
  hard once known and none of it is written down. Fix: `templates/ui/` with a
  panel, a button, a list and a menu, each a document that runs as-is against
  the current engine, referenced from CONTRIBUTING. A working example is worth
  more than a description, to a person and to a model alike.

  Delivered in `templates/ui/`: shared component styles, standalone button,
  list and panel examples, a native RmlUi panel template, and an animated menu
  with a separate JS controller. Authoring and validation commands are in
  CONTRIBUTING. This does not close the gamepad verification item below.

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
