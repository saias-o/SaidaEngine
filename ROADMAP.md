# SaidaEngine — Roadmap

Updated: 2026-07-24. This file is the engine's **single backlog**: everything
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
   (mesh + light + shadow + tonemap).
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
  ids — every AssetID stored in a scene (`skyboxTexture`, mesh references) is
  left dangling, silently. Fix by design: honour the failure, refuse to
  overwrite a document the engine could not read, and either migrate the entry
  ids or stop the load with a diagnostic naming the documents that reference
  them. SPEC section 13 documents the divergence meanwhile.

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

## 6. P2 — Out of scope, kept as a decision

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

## 7. Closed decisions — do not reopen without a new reason

- **Three `ReflectedTypes*` lists** (`.cpp`, `Player`, `Web`) kept distinct. The
  targets neither compile nor link the same subsystems (physics/Jolt, audio,
  QuickJS, scenario deliberately absent from some Web variants): a single source
  would force per-type `#ifdef` or unlinked symbols. `runtimeTypeMatrix`,
  verified at boot, remains the parity guardrail. The identical
  `registerBehaviour<T>`/`registerNode<T>` templates stay local: their small
  duplication is more readable than a cross-cutting header with no invariant.
- **MCP (`SAIDA_ENABLE_MCP`)** is an assumed V1 capability of the Saida
  workshop, ON by default, covered by a catalog test and a real TCP smoke test.
- **`EditorShell`** is not created: `EditorUI` is the final shell. A mirror
  class would have added neither state nor invariant.
- **`src/rhi/`** (383 lines) stays as is: it is the repository's cleanliness
  reference.
- **Assumed unmet metrics**: the V1 refactor aimed at 0 files > 800 lines and a
  largest file ≤ 600 lines; we stayed at 9 files > 800 lines with `Renderer.cpp`
  at the top. These goals were not "solved" by artificial compression or by
  extraction without an invariant — they remain open in §2.

## 8. Saida platform

The web platform, backend and operations live in
[`saias-o/saida`](https://github.com/saias-o/saida) and carry their own roadmap
as well as the global production go/no-go.
