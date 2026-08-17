# Instructions for LLM agents

These instructions apply to every automated coding agent working in this
repository.

## Canonical sources

1. Read [SPEC.md](SPEC.md) before changing architecture, contracts, formats,
   platform behavior or release procedures.
2. Read [ROADMAP.md](ROADMAP.md) before selecting or closing work.
3. Follow [CONTRIBUTING.md](CONTRIBUTING.md) for the development and
   verification workflow.
4. Treat `SPEC.md` as the truth of what exists and `ROADMAP.md` as the truth of
   what remains to be done.
5. Do not create competing planning or specification documents.

## Authorship

- Never add an LLM agent, model, vendor or agent tool as a commit co-author.
- Never add a `Co-authored-by` trailer for an AI agent.
- Do not claim human authorship or alter the configured human/service Git
  identity.

## Language

- Write all documentation, code comments, commit messages and user-facing
  repository text in English.
- Treat non-English documentation or comments as defects when they are in the
  scope of the current change.

## Change discipline

- Preserve unrelated user changes and work safely in a dirty worktree.
- Make focused changes; do not perform unrelated refactors.
- Update `ROADMAP.md` when a roadmap item is closed or a new proven blocker is
  introduced.
- Update `SPEC.md` when the implemented technical truth or a public contract
  changes.
- Do not weaken, delete or bypass tests merely to make a change pass.
- Do not edit vendored sources under `third_party`.
- Do not add silent fallbacks for missing capabilities or invalid durable
  content.
- Do not declare support without a real backend and an associated proof.

## Architecture and code quality

- Keep the editor, desktop, Web, XR and headless surfaces on the shared scene
  and gameplay model.
- Prefer RAII, explicit ownership and small components with clear
  responsibilities.
- Reject duplicated engine logic, hidden dependencies, unjustified global
  state and permanent magic values.
- Comments explain invariants, external constraints or non-obvious decisions;
  they do not narrate straightforward code.

## Formats and generated content

- Durable documents require the exact current schema and version.
- A format change updates its producers, loaders, fixtures and tests together.
- Never regenerate a frozen fixture merely to hide a divergence.
- Modify Witness generators rather than generated Witness outputs when the
  generator is the source of truth.
- Do not commit build directories, caches, crash reports, credentials or
  signing material.

## Verification

Run checks proportional to the change and report exactly what was run.

The native baseline is:

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

After a change to snapshots, SaidaOps, manifests, registries, scripting or
shared input, also rebuild and verify authoring WASM and the Web player.

Use the Witness harnesses for changes affecting export, runtime behavior,
gameplay, UI, persistence or restart:

```sh
./tools/witness_e2e.sh
./tools/witness_editor_play.sh
./tools/witness_editor_build.sh
./tools/witness_web_stage.sh
```

For changes affecting what is drawn — renderer, shaders, materials, lighting,
shadows, tonemap, HUD — also run the golden-image gate. It is the only check
that proves the engine still *looks* the same rather than merely still runs:

```sh
export VK_DRIVER_FILES="$(cygpath -w /c/msys64/ucrt64/share/vulkan/icd.d/lvp_icd.x86_64.json)"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
./tools/witness_golden_image.sh
```

It refuses to run outside Lavapipe: the reference is a software-rasterizer
image, and a real GPU differs from it across every lit surface. Re-record with
`--record` only for an intended visual change, and **look at the capture before
committing it** — the gate proves the frame did not change, never that it is
right.

To inspect the scene rather than gate it, aim the camera yourself. An exported
player accepts, on top of `--screenshot`:

```sh
"./Game.exe" --screenshot look.png --after-frames 30 \
    --camera-pos 3,0.25,3 --camera-look 0,0.3,0
```

Read the resulting PNG directly. This is the answer to defects that pass every
structural check — geometry not meeting the ground, wrong scale, a surface
floating — because they are invisible from the gameplay camera and obvious from
a grazing angle two metres away. Both flags are required together and the two
points must differ; a rejected viewpoint exits non-zero and writes no image.

Do not leave GUI applications or local servers running after verification.

## Release handling

- Release identities are immutable tags, commit SHAs, manifests and digests.
- Never move, overwrite or rebuild an existing beta, RC or stable tag.
- `latest` is only a convenience alias, never the release identity.
- Do not publish a stable release or claim Windows qualification without the
  required proofs and Authenticode verification.
- Record incomplete manual testing honestly in pre-release notes.

## Documentation

Non-obvious engine knowledge that is hard to discover from the code and has
already cost real time. Intended to move to a dedicated file later; until then,
append an entry whenever a task is slowed by something a future agent could not
have known in advance.

### Building/linking from Git Bash (not an MSYS2 shell)

The toolchain is MSYS2 UCRT64. Compiling objects works from any shell, but the
LINK step fails with `collect2.exe: error: ld returned 116 exit status` when
`ld` resolves the wrong runtime DLLs. Prepend the UCRT64 bin to `PATH` before
building:

```sh
export PATH="/c/msys64/ucrt64/bin:$PATH"
ninja           # or: cmake --build build --parallel
```

From a native MSYS2 UCRT64 shell this is already correct. The error is
environmental, never a code fault.

### Runtime `.scene` format vs authoring snapshot (easy to conflate)

- Runtime `.scene` files are loaded by `SceneSerializer` and use **numeric**
  node `id`s (`j["id"].get<NodeId>()`) plus a `schema`/`version` envelope, both
  equal to `kSceneVersion` (currently 2).
- `saida_tool validate-scene` validates the **authoring snapshot** format
  (`SceneSnapshot`), which uses **string** ids. It is a different schema: it
  reports a valid `.scene` file as all-errors, and would pass a snapshot the
  runtime rejects. Do NOT use `validate-scene` to check a `.scene` file.
- Verify a `.scene` by loading it: `SaidaEngine.exe --project <p> --play`, then
  check the log for `loaded scene from ...` with no `loadIntoScene:` errors.

### Project file format

- `.saidaproj` is JSON: `{ "schema":1, "version":1, "name", "engineVersion",
  "mainScene", "runtime":{maxFps,vsync}, "rendering":{...} }` (see
  `WitnessGame/WitnessGame.saidaproj`). `Project::load` `json::parse`s it — there
  is no INI fallback.
- The legacy `[SaidaEngine Project]` / `main_scene=` INI form does NOT load. The
  `main_scene=` key belongs to the runtime boot manifest (`game.saida`), a
  separate file — not the project.

### Animated glTF/glb characters

- A node with `"importedFrom": "model.glb"` re-imports that file at scene load
  (`SceneSerializer` → `GLTFLoader::load`), rebuilding the mesh, rig, `Animator`
  and clips; its serialized children stay empty on disk.
- The `Animator`'s clips come only from that ONE file. Clip lookup — including
  through a `.sgraph` — strips the `file#` prefix and resolves against clips
  already loaded on the Animator; it never loads an external clip file. To use
  animations that were exported into separate files, MERGE them into one glb
  (identical mesh/skeleton: append the second file's animation, its accessors,
  bufferViews and buffer bytes; channel target node indices stay valid when both
  files share the same node order).
- `GLTFLoader` attaches one `Animator` per skinned mesh. A character split into
  several skinned meshes (e.g. body + hat) yields several Animators, so a
  controller must drive them all (`Node::findBehavioursInChildren<Animator>`).
- Inspect clip names/durations with `saida_tool inspect-anim <file.glb>`.

### Third-person CharacterBody checklist

- `CharacterBehaviour` attaches to a `CharacterBody`; it reads WASD/ZQSD (GLFW
  key positions already cover AZERTY), jumps on Space, sprints on Shift.
- It requires: a floor with a collider (gravity is applied), a `Camera` in group
  `camera` (movement is camera-relative), and the body in the group the camera's
  `CameraFollow.targetGroup` names (`player`).
- `CollisionShape` capsule `height` is the FULL height, caps included:
  `halfCylinder = height/2 - radius`.

### Imported model forward axis

- `CharacterBehaviour.faceMovement` assumes the model's forward is local `-Z`.
  Many glTF/glb characters (typical Blender exports) face `+Z`, so the character
  turns to face the camera instead of the direction of travel. Only the visible
  facing is wrong — camera-relative movement is still correct; do not "fix" it by
  inverting input or the shared behaviour.
- Correct it in the scene, not the behaviour: put a 180° Y rotation on the model
  container node (the one that carries `importedFrom`), i.e. quaternion
  `[x,y,z,w] = [0,1,0,0]`. Diagnose by what the character shows at rest — its
  face means it needs the flip, its back means it is already right.

### Asset registry is engine-managed and regenerated

- `<project>/asset_registry.json` is rewritten by the engine every time it loads
  the project. Ids are derived from each asset's content hash, so they are
  stable/idempotent across runs but do NOT preserve hand-authored ids: the engine
  drops ids it does not recognize and re-keys assets by path+hash. (`BeachDemo`'s
  `gen_beach.py` `SKY_ID = 4242424242` is exactly such a placeholder.)
- Consequence: a scene that references a texture by a hand-picked id (e.g.
  `skyboxTexture`) loses it after the first engine run. Reference assets by the
  id the engine assigns, or expect the registry to be regenerated. Treat registry
  changes produced merely by running the engine as churn, not as edits to commit.

### "Open Project" does not list every project (and ignores the Hub)

- The editor's Open Project dialog does NOT read the Hub's registry. It scans one
  root recursively for `*.saidaproj`, and that root defaults to
  `SAIDA_PROJECT_ROOT`, which CMake sets to `CMAKE_SOURCE_DIR` — the engine
  checkout. A project kept anywhere else is simply absent from the list, with no
  error to explain it.
- Fix it in the dialog itself: the "Search root:" field at the top takes any path;
  type the project's folder and press Enter (or Scan). Point it at the project
  folder rather than a whole home directory — the scan is recursive and only
  prunes `build`, `third_party`, `.git`, `node_modules` and dot-directories.
- The Hub (`SaidaEngineHub.exe`) is the one that reads the registry,
  `%APPDATA%/SaidaEngine/hub.json` (`{"projects":[{"name","path"}]}`); `path` is
  the project FOLDER, which `Project::load` resolves to its single `.saidaproj`.
  `--project <folder>` works the same way and bypasses both.

### `ignoreSelf` does not protect a raycast from a CharacterBody

- `physics.raycast(..., {ignoreSelf: true})` still hits the character it is cast
  from: the option resolves the node's `CollisionObject`, while a `CharacterBody`
  is also backed by an inner body that answers queries (SPEC 5.1).
- The symptom is silent and convincing: `distance = 0`, a perfectly horizontal
  normal, and the character's own node in `hit.node`. A controller probing for a
  wall therefore believes it is against one on every frame, and a wall kick
  fires anywhere in the open.
- Until the option covers the inner body, start the ray OUTSIDE the capsule
  (`origin + direction * (radius + margin)`) and drop hits at distance ~0. Print
  `hit.node.getName()` when a query behaves oddly — it names the culprit in one
  line.

### A rejected asset registry is rewritten, losing every AssetID

- `Project::load` calls `AssetRegistry::load` **without checking its result**,
  then `sync()` and `save()` unconditionally. A registry rejected by the schema
  guard (an older `schema`, a hand-edit that broke the envelope) is therefore
  replaced by a fresh scan: same paths, brand new random ids, old file gone.
- Nothing downstream is told. A scene's `skyboxTexture`, or any AssetID written
  into a durable document, now points at an id that no longer exists — the
  skybox simply stops drawing. The only clue is one line early in the log:
  `AssetRegistry: unsupported asset registry schema vN`.
- So: after any change to the registry's schema, expect every project's ids to
  be regenerated on first open. Resolve assets **by path** through the registry
  when generating scenes, never by pasting an id — ids come from
  `generateID()`, which is random, and only persist while the file survives.

### One physics body collides with ONE mesh

- `CollisionShapeNode.cpp`'s `findMesh` returns the **first** drawable mesh in the
  body's subtree, and `Auto`, `ConvexHull` and `Mesh` all build from that single
  mesh. A body whose subtree holds several meshes therefore collides with one of
  them and ignores the rest.
- This bites hardest on imported levels: a glTF scene is one node carrying dozens
  of meshes, so `StaticBody` + `CollisionShape(Mesh)` + `importedFrom` gives
  collision on a single piece. Nothing warns — the shape built correctly, just
  not around the geometry you meant — and the player falls through the level.
- Until the engine builds a compound from every mesh under a body, split the
  level into one glTF per piece and emit one body per piece.
- Related: the `Mesh` and `ConvexHull` shape types ARE implemented (triangle mesh
  is static-only, hull is dynamic-capable). Only their fallback to `Box` is
  logged, so a silent result means the real shape was built.

### Binary assets use Git LFS (not gitignore)

- `.gitattributes` routes `*.glb`, `*.gltf`, `*.png`, `*.fbx`, `*.tga`, etc.
  through **Git LFS**; these are tracked, not ignored. Never assume a `.glb`/
  texture is git-ignored. Commit such assets through LFS, or, when asked to keep
  them out of the repo, leave them untracked and say so — do not silently add
  them and do not rewrite `.gitignore`/`.gitattributes` policy without being
  asked.

### A character that is only a shadow (skinned mesh, colour pass, alpha)

- Symptom: the skinned mesh casts a correct, animated shadow and is otherwise
  invisible; props next to it render fine. The cause is the material, not the
  skinning: `alphaMode: "MASK"` with `baseColorFactor` alpha `0` cuts the mesh
  out of the colour pass while the shadow pass, which ignores alpha, still draws
  it. Blender's FBX importer leaves Principled **Alpha at 0** on some rigs, and
  the glTF exporter faithfully writes that out.
- Diagnose in one step: dump the GLB's `materials` and read `alphaMode` and the
  fourth component of `baseColorFactor`. Do not go looking at culling, bone
  palettes or bounds first — a mesh that is culled has no shadow either.

### Kenney kits: two defects that stop the loader cold

- **Scene roots with a parent.** The nature kit is exported by UniGLTF, which
  wraps the model in a `tmpParent` node and then lists the CHILD as the scene
  root. That is invalid glTF and `cgltf_parse_file` rejects the whole file with
  `cgltf_result_invalid_gltf` before any mesh is read. Repair the asset (point
  each scene at its parentless nodes), never the loader.
- **sRGB in a linear field.** `baseColorFactor` is linear by spec, but the kits
  store the artist's sRGB hex there — `colorRed` is literally `#E04A4F` read as
  bytes. Rendered correctly that comes out pastel, and the whole kit looks
  washed. Convert once in the asset. Both repairs live in
  `VerticalSlice/tools/`; they are idempotent and marked in `asset.extras`.

### Aiming a third-person shooter: two sign traps

- `CameraFollowBehaviour::initialPitch` is inverted from the intuition (its
  header says so): a POSITIVE pitch drops the rig BELOW its target and aims it
  upward, so every shot sails over. `0` is the level, neutral start.
- `shoulderOffset` displaces the rig sideways while it keeps looking at the
  pivot, so the camera axis no longer points where the character faces. With a
  centre-screen crosshair the shot then lands beside whatever the player is
  pointing at. Either keep the offset and resolve the crosshair with a ray from
  the camera before firing from the muzzle (both — the ray gives convergence),
  or set the offset to 0.

### A NodeRef outlives what it points at

- Every `NodeRef` method except `valid()` throws
  `ReferenceError: NodeRef target no longer exists` once the node is freed. A
  reference captured in a closure — a projectile remembering what it will hit, a
  timer touching a child of a node it is about to free — will fire that error
  every frame it retries. Guard with `ref.valid()`; it is the one method that
  answers `false` instead of throwing. `queueFree()` takes the whole subtree, so
  a child captured earlier dies with its parent.

### A follow camera that flickers between two distances

- Symptom: every frame the view alternates between very close to the character
  and a little further out, and the scene reads as two images at once. It is not
  tearing and it is not a frame-pacing problem — it is the de-occlusion probe
  feeding back into itself, and it shows up wherever the occluder is close to the
  pivot, i.e. as soon as the rig is pitched down into the ground.
- Two rules keep a deoccluder stable, and `CameraFollowBehaviour` used to break
  both: **probe the segment the rig WANTS to occupy** (pivot → desired), never
  the one it currently occupies — a pulled-in camera otherwise casts a ray too
  short to reach the wall that pulled it in — and **never write the correction
  into the smoothing state**, or it becomes its own input on the next frame.
  Carry the easing on the free *length* instead: snap it down, damp it back up.
- Measure rather than eyeball it: a screenshot cannot show an alternation, and
  the swing can be as small as 0.3 m while still reversing 99% of frames. Sample
  the pivot-to-camera distance per frame and count sign changes in its delta —
  `VerticalSlice/scripts/e2e_camera.js` does exactly that (118/119 reversals
  before the fix, 0 after).

### A follow camera that shakes while the character runs

- Different cause from the flicker above, same family: the rig AMPLIFIES an
  uneven frame clock instead of absorbing it. `CameraFollowBehaviour` measures
  the target's speed as a one-frame finite difference, `(pos - lastPos) / dt`,
  and a position delta divided by a jittery frame time is a noise amplifier —
  between a 7 ms and a 24 ms frame a perfectly steady run reads as a 3x speed
  swing. Everything downstream of it (look-ahead, the speed-driven fov, velocity
  recentring) then shakes the whole image while the character itself is moving
  evenly. The estimate is now low-passed before anything visible consumes it.
- The measurement that localises this in one run: sample per frame the target's
  step, the camera's step and `time.delta`, and compare their *unevenness* (mean
  absolute change between consecutive steps, over the mean step). The target's
  should equal the frame clock's; a camera above it is amplifying.
  `VerticalSlice/scripts/e2e_run_jitter.js` gates exactly that — 1.59 against an
  input of 0.61 before, 0.53 after.
- Note what this does NOT fix: the frame clock itself. `dt` still ranges 7–24 ms
  around a 10 ms mean here, and that residual unevenness is frame pacing, not the
  camera. Do not chase it inside gameplay code.

### WebCanvas callbacks and scene changes have two re-entrancy traps

- A JavaScript DOM mutation made from an RmlUi listener must not call
  `Rml::Context::Update()` synchronously. Focus and pointer listeners are
  themselves dispatched from an update; re-entering it repeats the listener
  until QuickJS reports `Maximum call stack size exceeded`, then can overflow
  the native stack. Mark the canvas dirty and let the renderer's normal UI
  update make the mutation visible later in the same frame.
- `UIInteractionSystem` keeps hovered, pressed, focused and touch targets across
  frames. A deferred `changeScene` destroys the old `WebCanvasNode` after input
  handling, so those raw pointers must be discarded when the hierarchy version
  changes before calling `fireMouseLeave()` or forwarding the next key. The
  characteristic crash resolves to `Rml::Context::ProcessMouseLeave()` on the
  context owned by the already-destroyed menu.
- Do not gate `ProcessMouseMove()` on `WebCanvasNode::hitTest()`. RmlUi needs
  every move inside the canvas to update its own hovered element; pre-filtering
  moves creates direction-dependent hover hysteresis. Route movement by canvas
  geometry, then use `hitTest()` only for click, scroll and input-capture policy.

### The editor viewport is the dockspace central node

- `EditorUI::drawDockspace` records the drawable scene rectangle from ImGui's
  dockspace central node. That rectangle is shared by rendering, picking,
  gizmos and screen-space UI input.
- Do not replace it with `ImGui::GetMainViewport()->WorkPos/WorkSize` in a
  panel or gizmo pass. The main viewport includes docked panels, so the scene
  and WebCanvas render behind those panels while input appears offset from the
  visible center area.
- Diagnose this mismatch by checking whether a full-screen WebCanvas is clipped
  under the Scene Tree or Inspector. The correct fix is to preserve the central
  dock rectangle, not to compensate individual mouse or CSS coordinates.

### A colour transition can look exactly like a displaced hover hitbox

- If hover appears only at the far edge of every control, and that edge reverses
  when pointer travel reverses, first inspect the transition duration. A
  180 ms transition on every hover colour made a correctly targeted button
  become visibly highlighted only after the pointer had crossed most of it.
- Verify the distinction by checking the RmlUi `hover` pseudo-class at the
  element's border-box centre. If it changes immediately while pixels fade,
  input geometry is correct and coordinate compensation will make the system
  worse.
- Keep the primary pointer affordance immediate. Animate secondary decoration
  or hover exit when desired; do not add JavaScript hover classes to compensate
  for a visual transition.

### `tree.changeScene()` confirms queuing, not loading

- The QuickJS binding returns `true` once its argument converts to a string and
  the deferred request is queued. It does not resolve or load the scene before
  returning.
- A missing or rejected scene therefore fails later while gameplay has already
  received a truthy result. Do not consume an irreplaceable portal, key or
  objective solely on that return value. Keep a retry path until the engine
  exposes transition completion/failure.
- Verify a transition by observing a marker in the destination scene after at
  least one deferred-operation frame. `VerticalSlice/scripts/e2e_level2.js`
  follows this pattern.
