# SaidaEngine — Roadmap

Updated: 2026-07-27. This file is the engine's **single backlog**: everything
that remains to be done, deferred or decided for later. It does not describe
what exists — the technical truth is in [SPEC.md](SPEC.md), and getting started
is in [README.md](README.md).

Rule: nothing is checked off here without the run, commit or exact corpus that
proves it. Closed work (V1 gates, V1 refactor) lives in the Git history and in
the corresponding contracts of `SPEC.md`.

## 0. Status

The **V1** effort is complete on the engine side: gates P0.1 through P0.6 are
closed, the V1 refactor derived from the quality audit is closed
(ResourceManager 1102 → 414 lines, EditorUI 1933 → 384 lines, McpBridge
1401 → 77 lines, split of `src/scene/`, named Input/Renderer constants), and
final validation has passed: clean native build, 70/70 CTest, three Web builds
(player, runtime, authoring WASM), Witness E2E, `witness_editor_play`,
`witness_editor_build`, Web staging and a real MCP TCP smoke test (45 tools).

The public beta cycle is open. The remaining Windows distribution work before a
qualified stable publication is tracked in §1.

Reminder of the release criteria: the same WitnessGame must run in the editor,
standalone desktop and Web; old projects must migrate or be rejected without
corruption; memory must stay bounded; the published limitations must match the
observed behavior; the artifacts must come from a clean commit.

## 1. Before stable publication — Windows distribution and signing

- [ ] Produce a portable Windows editor package. The raw Beta 1 editor
  executable still resolves compiled shaders, fonts and editor assets through
  configure-time absolute paths, so copying `SaidaEngine.exe` and `glfw3.dll`
  alone is only a developer artifact. A release package must resolve resources
  relative to the executable, ship the required files and pass from a clean
  extraction directory with no source or build tree.
- [ ] Give the editor a project-discovery root that survives distribution. The
  Open Project dialog scans `SAIDA_PROJECT_ROOT` (`CMAKE_SOURCE_DIR`), so a
  shipped editor looks for projects inside an engine checkout the user does not
  have, and silently lists nothing. It also ignores the Hub registry that already
  knows where projects live. Fix by design: default to the last opened project's
  folder, persisted between sessions, falling back to the OS documents folder,
  and share the Hub's registry instead of scanning a build-time path.

- [ ] Add and verify Windows version resources on the engine executables:
  `ProductName` must be `SaidaEngine`, and `FileVersion`/`ProductVersion` must
  carry the same release version. The raw Beta 1 editor executable has no
  VERSIONINFO resource.

- [ ] Produce the Windows installer **Authenticode-signed** with the publishing
  key, then inventory the SHA-256 of the signed bytes.

The rest of the release chain is closed: mandatory CI (native build, 70 tests,
V1 corpus, deterministic fold, desktop and Web Witness); `saida_tool`, Web
player and authoring WASM published as pinned artifacts; byte-identical
Windows/Linux proof on the fold fixtures; byte-reproducible archives and NSIS
Witness installer **before signing**, recursive validation of DLL imports,
inventoried uninstall, documented immutable rollback; crash logs and symbols
tied to the version; SPDX SBOM + GPL/third-party notices + license/asset
inventory. The portable editor package, its Windows metadata and signing remain
separate publishing work.

## 2. Structural code debt

Finding of the quality audit: the codebase is healthy and disciplined (near-zero
TODO/FIXME debt, systematic fail-closed policy, frozen format corpus,
capabilities verified at startup). The points below break nothing; they will
cost more and more as the engine grows — the GPU-driven flag, XR and advanced
rendering are precisely blocked behind the first line of the table.

### 2.1 Remaining god classes

| File | Lines | Mixed domains |
|---|---|---|
| `src/render/Renderer.cpp` | 1975 | pipelines, descriptors, GI, tonemap, GPU-culling, shadows, XR, frame — see §3 |
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
(213 lines, `src/render/Renderer.cpp:704`), `recordCommandBuffer`
(164, `:1328`), `drawFrame` (118, `:1492`), `rebuildGlobalSet` (108, `:477`).
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

`src/render/Renderer.cpp` (1975 lines) is the largest god class and the
highest-value decomposition: it unblocks XR, the GPU-driven flag and lightmaps
by making them testable in isolation. It is **deliberately deferred** — it was
taken out of the V1 refactor because it is not safe to do mechanically.

### Target decomposition (to execute *when the visual safety net exists*)

| Unit (state/invariant) | Methods taken over |
|---|---|
| **PipelineCache** — pipelines & layouts; rebuilt on format/swapchain change | `createGlobalSetLayout`, `createPipeline`, `createWebCanvasWorldPipeline`, `createCullingPipeline`, `createTonemapPipeline`, `createXrPipelines` |
| **FrameDescriptors** — global descriptor sets + per-frame-in-flight UBO | `createUniformBuffers`, `createGlobalDescriptorSets`, `rebuildGlobalSet`, `updateGlobalShadowDescriptor`, `updateUniformBuffer` |
| **GIRenderer** — GI descriptors + dirty cadence/signature | `shouldUpdateRealtimeGI`, `giDirtySignature`, `updateGIDescriptors`, `updateEnvironmentDescriptor` |
| **TonemapPass** — HDR targets + tonemap pipeline/descriptors | `createHdrResources`, `cleanupHdrResources`, `createTonemapPipeline`, `updateTonemapDescriptorSet`, `tonemapPushConstants`, `recordTonemapPass` |
| **GpuDrivenCuller** — indirect draw buffers + culling pipeline (behind the flag) | `createGpuDrivenBuffers`, `uploadGpuDrivenDraws`, `featureDraws`, `buildFeatures`, `recordFeatures` |
| **XrRenderer** — XR targets/pipelines + multiview UBO | `createXrTargets`, `cleanupXrTargets`, `updateXrTonemapDescriptorSets`, `updateUniformBufferXr`, `recordXrScenePass`, `recordXrWorldWebCanvases` |
| **SceneGatherer** — draw-list + lighting UBO from the scene | `gatherScene` (split), `recordMeshDraws`, `recordWorldWebCanvases`, `recordShadowPasses` |
| **Renderer** (thin remainder) — frame orchestration | `setViewportRect`, `clearViewportRect`, `activeRenderRect`, `drawFrame`, `recordCommandBuffer` |

### Why it is risky (do not do it blindly)

1. **No automatic check of rendering correctness.** Witness E2E confirms that
   the HUD and the scene *display* (non-zero pixels, no crash), but NOT
   colorimetric/geometric correctness. A subtle error — wrong descriptor,
   inverted pass order, changed shadow bias, wrong blend — would pass the net
   and break rendering **silently**.
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

1. **Establish a visual verification net. Absolute prerequisite.** Without it,
   refactor nothing in the Renderer. A deterministic rendering harness that
   captures a reference frame (*golden image*) of a fixed scene and compares it
   pixel-by-pixel (bounded tolerance), run on Lavapipe in CI **and** on a real
   GPU. The HUD already has computed-pixel assertions
   (`saida_ui_corpus_tests`) — extend that idea to a full scene frame
   (mesh + light + shadow + tonemap). The engine-side capture this needs is the
   same one §6.3.1 asks for (`SaidaEngine --screenshot`); build it once. The
   encoder half is done and reusable: `core/PngWriter` writes deterministic
   PNGs with no third-party dependency, so a golden image is byte-comparable.
2. **Extract one unit at a time, leaf-first order**, one commit + one visual
   check per unit: `TonemapPass` (clean boundary: input = HDR target,
   output = swapchain) → `GpuDrivenCuller` (behind its flag) → `XrRenderer`
   (guarded) → `GIRenderer` → `PipelineCache`/`FrameDescriptors` → thin
   `Renderer` last.
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
HUD rendering. It **does not cover**: pixel-accurate rendering, editor GUI, XR,
the semantics of MCP mutations. Any session touching these areas must be
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

- [ ] XR: validate targeted headsets/runtimes, controllers and hand tracking.
- [ ] XR: multiview MSAA/resolve, ImGui overlay and a real anchors backend.
- [ ] Physics: make `ignoreSelf` cover a `CharacterBody`. The option resolves
  the caller's `CollisionObject` body, but a character is backed by an inner body
  as well, so a controller's own forward ray hits its capsule at distance 0 with
  a horizontal normal — every frame looks like a wall. Fix by design: resolve the
  inner body too, and make the option's contract testable (a query from a
  character must never return the character).

- [ ] Physics: complete queries, constraints (slider, cone, motors) and
  diagnostics.
- [ ] Formats: stop rewriting a rejected `asset_registry.json`. `Project::load`
  ignores the loader's verdict and runs `sync()` + `save()` anyway, so a registry
  the schema guard refused is replaced by a fresh scan with brand new random
  ids — every AssetID stored in a scene (mesh references) is left dangling,
  silently. Fix by design: honour the failure, refuse to overwrite a document the
  engine could not read, and either migrate the entry ids or stop the load with a
  diagnostic naming the documents that reference them. SPEC section 13 documents
  the divergence meanwhile. A scene's `skyboxTexture` is no longer exposed to it:
  both scene readers now accept a project-relative path, which the registry
  re-resolves after any rescan (SPEC 3.1).

- [ ] Physics: make a body collide with **every** mesh under it, not just the
  first. `CollisionShapeNode`'s `findMesh` picks a single mesh, so an imported
  level — one node, dozens of meshes — silently collides on one piece and the
  player falls through everything else. The workaround (one glTF and one body per
  piece) pushes level authoring into the asset pipeline and multiplies bodies for
  what is one static level. Fix by design: build a Jolt compound (or a merged
  `MeshShape`) from the whole subtree, and log when a body's geometry is only
  partially covered instead of succeeding quietly.
- [ ] Animation: extended API (scrub, JS root motion) and BVH retargeting.
- [ ] Stabilize the GPU-driven flag and benchmark the classic path, bindless,
  indirect draw and compute culling on a reproducible corpus.
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

- [ ] Scene loading: stop dropping a subtree on an unknown node type.
  `SceneSerializer::deserializeNode` logs an error and returns `nullptr` for an
  unregistered type; the children loop simply does not add it, and the load still
  reports success. One typo in a `"type"` therefore deletes the node **and every
  descendant** while the game boots normally. Same family: `ensureUniqueIds`
  regenerates a duplicate or invalid id in silence rather than refusing the
  document. Both contradict the rule the repository imposes on itself — *no
  silent fallbacks for invalid durable content* — which the schema envelope
  already honours a few lines away. Fix by design: fail the load with a
  diagnostic naming the offending type and path, as `acceptSceneDocumentVersion`
  does.

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

- [ ] Tooling: capture a real frame from the engine. `SaidaEngine` can run
  headless (`SAIDA_WINDOW_HIDDEN=1`) but cannot write what it drew, so the
  composite of 3D and UI — the thing the player actually sees — is verifiable
  by nobody except a human looking at a screen. Fix:
  `--screenshot <png> [--after-frames N]`, writing the presented frame after a
  deterministic number of frames. **This is the same capability §3 declares an
  absolute prerequisite to touching `Renderer.cpp`**: build it once, and both
  the UI loop and the golden-image net become possible.

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

  Left open deliberately: tags outside the specified list (`header`, `nav`,
  `ul`, `li`, `article`, …) keep RCSS's `inline` default and hit the same trap.
  Extending the list is a contract change, so it is a decision rather than an
  oversight.

- [ ] UI: give a screen-space canvas a reference resolution. A screen-space
  `WebCanvasNode` that fills the viewport is resized to the real window
  (`UIRenderer::gatherUI`), while the scene keeps declaring 1920x1080. Absolute
  pixel geometry — the natural way to lay out a fixed-size canvas, and what the
  declared size invites — therefore lands off-centre on every other window
  size, and anything past the window height is clipped by the context scissor
  with no warning. Fix: `referenceWidth`/`referenceHeight` plus a `scaleMode`
  (`stretch` = today's behaviour, `fit` = letterboxed reference, `expand`),
  which is what Unity's CanvasScaler and Godot's stretch mode exist for. It
  makes pixel layout *correct* instead of merely tempting, and `fit` gives an
  author one resolution to reason about.

- [ ] UI: wrap texture coordinates in the CPU RmlUi backend.
  `decorator: image(x.png repeat)` does not tile: the backend clamps instead of
  wrapping, so a tiled surface draws one copy and then smears its edge texels
  across the rest of the element. `tiled-box` and `ninepatch` — the decorators
  a framed panel actually wants — rest on the same mechanism. Fix: implement
  wrapping in the sampler; if it is declined, reject the `repeat` keyword with
  a diagnostic instead of drawing something plausible and wrong.

- [ ] Tooling: lint a UI document. RCSS reports a failed declaration as one
  warning in a log nobody reads at authoring time, and the two mistakes met
  here are both mechanically detectable: `rgba()` written with a 0-1 alpha
  (RCSS alpha is 0-255) parses as nothing and the property silently vanishes;
  an element carrying `width`/`height`/`text-align` while computing to
  `display: inline` is always a bug. Fix: `saida_tool validate-ui <document>`
  reporting unparsed declarations, unresolved asset references and that
  inline-with-box-properties pattern, on the model of `validate-scene`.

#### 6.3.3 Content pipeline and authoring surface

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

- [ ] UI: templates for the surfaces every game needs. Authoring the menu meant
  rediscovering, by reading engine and RmlUi sources, how a panel, a button, a
  focus state and a keyboard-driven selection are expressed here. None of it is
  hard once known and none of it is written down. Fix: `templates/ui/` with a
  panel, a button, a list and a menu, each a document that runs as-is against
  the current engine, referenced from CONTRIBUTING. A working example is worth
  more than a description, to a person and to a model alike.

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

- network multiplayer, large terrains and genre frameworks;
- generalized SIMD animation, massive pose sharing and GPU crowds;
- Radiance Cascades and advanced GI research;
- RmlUi GPU backend as long as the CPU backend meets the budgets;
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
