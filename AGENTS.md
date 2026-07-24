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

### Binary assets use Git LFS (not gitignore)

- `.gitattributes` routes `*.glb`, `*.gltf`, `*.png`, `*.fbx`, `*.tga`, etc.
  through **Git LFS**; these are tracked, not ignored. Never assume a `.glb`/
  texture is git-ignored. Commit such assets through LFS, or, when asked to keep
  them out of the repo, leave them untracked and say so — do not silently add
  them and do not rewrite `.gitignore`/`.gitattributes` policy without being
  asked.
