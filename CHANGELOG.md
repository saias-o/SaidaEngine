# Changelog

All notable changes to SaidaEngine. Versions follow the scheme described in
[CONTRIBUTING.md](CONTRIBUTING.md#versioning): each manual test and correction
cycle receives its own immutable beta tag.

## v1.0.0-beta.2

32 commits since `v1.0.0-beta.1`. Most of what follows was found by authoring
real content against the engine rather than by reading it — a vertical slice, a
main menu and two small games — which is why so many entries are defects that
produced a wrong picture with no error attached.

### Rendering

- **Skinned meshes now cast their animated pose.** `shadow.vert` declared
  neither `JOINTS_0` nor `WEIGHTS_0`, so every skinned caster was drawn from its
  bind pose. A character could appear to have no shadow at all.
- **Diffuse image-based lighting has a direction.** The environment's diffuse
  irradiance was a single sample of a coarse mip, which is very nearly the
  environment average and identical for every normal — a floor and a ceiling
  received the same light, washing the scene flat.
- **A scene's rendering settings are read in one place.** `Scene::deserialize`
  and `SceneSerializer::loadIntoScene` each carried a copy of the
  `SceneSettings` field list and the two had drifted: different defaults for the
  same key, a different alpha on colours, and only one accepted an asset path
  where the other demanded a numeric ID. A scene naming its skybox by path threw
  a JSON type error that abandoned the whole load. Naming assets by path now
  works and survives a re-keyed registry.
- Scenes may drive rendering, animation and imports from data rather than
  requiring native gameplay code.

### Character and camera

- **The follow camera no longer fights its own inputs.** Two defects, both
  measured rather than eyeballed. Pitching the rig into the ground made the view
  alternate every frame between close and far, because the deoccluder probed the
  segment the camera *currently* occupied and wrote its correction back into the
  smoothing state. Sign changes in the pivot-to-camera distance went from 118 of
  119 frames to 0. Running shook the image separately: target speed was a
  one-frame finite difference over a jittery clock, so a steady run read as a 3x
  speed swing that look-ahead, the speed-driven FOV and velocity recentring then
  applied to the whole view. Jitter went from 1.59 to 0.53 against an input of
  0.61.
- **Every skinned mesh on a character is driven.** An imported character can
  carry several skinned meshes (body, hat), each with its own `Animator`. Only
  the first was driven, so the rest stayed stuck on whatever clip the import
  started with.
- **A partial stick deflection walks at a partial speed.** The left stick is
  bound as an analog axis, but callers reading `isActionHeld` got each axis
  thresholded at 0.5 — eight directions and nothing in between.
- **The right stick looks around.** The follow camera read the mouse and nothing
  else, so a gamepad could move a character but never turn the view. Scripts can
  now add a binding instead of replacing the whole table.
- The character controller and its camera are tunable from data. Feel was eight
  properties and a fixed solver, which pushed games into reimplementing both in
  script.

### User interface

- **A reloaded UI document gets a fresh JS context.** Document scripts run as
  global code, so re-evaluating them in the context they replace redeclared
  their top-level `let`/`const` and threw. That alone broke hot reload for any
  document whose script declared one.
- **UI documents resolve against the loaded project.** `assetPath()` points at
  the engine root in editor and dev builds, so a game's `ui/main_menu.html` and
  its stylesheets and images only resolved when they happened to sit inside the
  engine tree.
- **WebCanvas callbacks no longer re-enter the update that dispatched them.** A
  DOM mutation made from an RmlUi listener must not call `Context::Update()`
  synchronously: focus and pointer listeners are themselves dispatched from an
  update, so re-entering it repeated the listener until QuickJS reported a stack
  overflow, and could then take the native stack with it.
- **Input targets survive a scene change.** `UIInteractionSystem` held hovered,
  pressed, focused and touch targets as raw pointers across frames, and a
  deferred `changeScene` destroys the old `WebCanvasNode` after input handling.

### Editor

- **Viewport gizmo rework.** Click-to-select works with nothing selected —
  picking no longer sits behind the gizmo's "requires a selection" early-out —
  and ray picking collects every node under the ray, cycling through overlapping
  candidates on repeated clicks.
- **The profiler reports triangle counts.** `Scene/TotalTriangles` and
  `Renderer/FrustumTriangles`, counted once per active mesh instance. The
  frustum test is now shared with the renderer rather than duplicated, so the
  camera figure cannot drift from what was actually submitted. Collection runs
  only while the desktop profiler is open, and the collector is linked by the
  editor alone.
- **A Small interface size** for small displays, next to Editor Theme.

### Assets and import

- **A rejected glTF names its cause.** The loader reported only `error 4`. These
  failures are almost always an exporter quirk in content the engine did not
  produce, and the file is usually fixable — the invalid-glTF case now names the
  scene root that is also another node's child, which is what makes cgltf refuse
  a file before a single mesh is read.
- **`.hdr` is typed.** Textures already decoded it through `stbi_is_hdr`, but the
  asset registry classified it as `Unknown` — it is the equirectangular skybox
  and IBL source.

### Authoring

- **Optional standalone Python authoring SDK.**
- **Pong3D**, a game authored against the released engine using only a project,
  a scene and a script, without touching engine code.
- **Verdance**, a two-level third-person vertical slice that exercises the
  engine end to end: HDRI skybox with IBL, GI, SSAO, bloom, fog, cascaded
  shadows, skinned characters, particles, bodies and areas, UI, audio and
  QuickJS gameplay. Its second level, The Deep Wilds, spans 168 m by 160 m with
  4,467 vegetation instances and roughly 580,000 mesh triangles, and is the
  dense-geometry stress case. Its river uses the cartoon water style.
- **BeachDemo** converted from the legacy INI project form, which the current
  JSON project loader cannot read.

### Release engineering

- **One home for versions.** `kProductVersion` (human-facing release) and
  `kEngineVersion` (on-disk format contract) both live in
  `src/core/EngineVersion.hpp`, and nothing hard-codes a version elsewhere.
- **The vertical slice's art is declared.** 676 tracked assets — the Kenney
  kits, the audio and the Poly Haven sky — now carry a CC0-1.0 row each in the
  fail-closed compliance inventory, with the repairs noted where a file was
  modified.
- **An unattributed model was removed.** `BeachDemo/assets/models/Zleda.glb` had
  no recorded source, author or licence anywhere in the repository, so it was
  deleted along with the scene that existed to display it. Every tracked asset
  is now declared and distributable.

### Known limitations

- The Windows executable is **unsigned**. The SignPath integration and a
  publicly trusted certificate are not provisioned; see
  [CODE_SIGNING_POLICY.md](CODE_SIGNING_POLICY.md).
- The raw `SaidaEngine.exe` is a developer artifact rather than a portable
  distribution: the editor build resolves shaders, fonts and branding through
  absolute configure-time paths, and the Open Project dialog scans the engine
  checkout instead of the Hub registry.
- Scene loading re-parses a model's glTF once per instance. Meshes are
  deduplicated on the GPU, so this costs load time rather than memory, but a
  dense scene pays for it: The Deep Wilds re-reads about 50 MB of glTF from
  1.7 MB of distinct files.
