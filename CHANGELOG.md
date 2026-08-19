# Changelog

All notable changes to SaidaEngine. Versions follow the scheme described in
[CONTRIBUTING.md](CONTRIBUTING.md#versioning): each manual test and correction
cycle receives its own immutable beta tag.

## v1.0.0-beta.3

34 commits since `v1.0.0-beta.2`. Two themes dominate: a vehicle stack built by
driving it in a real city rather than by reading it, and the first machine-checked
way to look at what the engine draws. As in the previous cycle, most entries are
defects that produced a wrong picture or a wrong motion with no error attached.

### Vehicles

- **A raycast vehicle.** A `Vehicle` behaviour on a `RigidBody`: each wheel is a
  ray, and suspension, drive and grip are applied *at the contact*, so weight
  transfer falls out of the physics instead of being scripted on top of it.
  `PhysicsWorld` gained what that needs and could not express — impulses and
  forces at the centre of mass or at a world point, angular impulse and torque,
  plus `pointVelocity` and `effectiveMassAt`, the two contact quantities a
  correction has to divide by if it is not to over-correct.
- **Steering was mirrored.** The vehicle's local +X is its left, so a positive
  steer input and a positive heading rotation had to disagree in sign, and did
  not: steer -1 turned -152.9 degrees, steer +1 turned +152.8. The drawn wheels
  carried the same error and pointed away from where the car went. The test that
  should have caught it asserted only that opposite inputs turn opposite ways.
- **A long frame no longer launches a car.** The vehicle turns force into impulse
  by multiplying by the frame delta; while the city streamed, that delta reached
  0.63 s and the suspension became 89 kN.s over four wheels — 74 m/s upward on
  1200 kg, and a parked car climbed to 28 m on its own. `PhysicsWorld` never
  advances more than its fixed step times its substep ceiling, so the excess was
  force paid for time the world does not simulate. That bound is now one named
  constant the two share, and the vehicle clamps to it.
- **Cars sit on their axle line.** `centerOfMass` comes from the wheel manifest,
  at wheel-centre height. Left at the centre of a collision box that has to cover
  the roof, every car in the city tipped below the grip its own tyres could pull
  and rolled over in any hard corner.
- **Self-righting works from any attitude.** The righting torque turned about the
  car's length — the axis it rolled over on — which covers the commonest way to
  strand a car and nothing else; any other attitude just ground the car where it
  lay. It now turns about `up x worldUp`, falling back to the length only when
  the car is exactly inverted and there is no shortest way back.
- **Getting in and out.** `vehicleDrive`, `vehicleBrake`, `vehicleHandbrake`,
  `vehicleInput` and `vehicleState` on both `node` and `NodeRef`, so a driver
  need not be the vehicle's own script. `vehicleInput` exists because a character
  and a vehicle read the *same* movement actions and exactly one may be
  listening: a parked car left listening steers itself off the kerb whenever the
  player walks past. Turning it off also releases whatever the keyboard was
  holding, so a car cannot be handed over mid-throttle. Exit is refused above
  2 m/s and leaves the handbrake on.
- **All 32 cars in the city open**, driven by one `driver.js` on a node of its
  own rather than a script per car — 32 scripts would race to answer one key
  press, whereas one driver holding at most one car cannot race anything.
- **The Web player drives too.** The runtime matrix marked the vehicle Required
  on Web and the player compiled it, but nothing had ever executed one there:
  the raycasts, the impulse API and the fixed-step interaction were unproven.
  `e2e_drive.js` now passes in the Web player as on desktop (resting wheel
  height 0.403 against 0.398).

### Seeing what the engine draws

- **`SaidaEngine --screenshot <png> [--after-frames N]`** writes frame N and
  exits. Three things had to exist first: the swapchain never requested
  `TRANSFER_SRC`, so the presented image was not copyable at all; the RHI had no
  image-to-buffer copy; and the channel order is now resolved from the actual
  swapchain format, with anything outside R8G8B8A8/B8G8R8A8 refused rather than
  written with swapped channels.
- **The same command twice gives the same pixels.** A capture was reproducible by
  frame *number* but not by frame *content*: an asset finishing on frame 3 in one
  run landed on frame 4 in the next, and the frame clock read the wall clock.
  Both produce a plausible image of the wrong instant — exactly what a pixel
  comparison cannot diagnose. `CaptureScheduler` now waits until nothing is
  queued, in flight or awaiting finalization, then counts frames from there.
- **A golden-image gate**, `tools/witness_golden_image.sh`: frame 30 of the
  WitnessGame hub scene compared against a committed reference, exactly. It is a
  **local** check and deliberately not in CI — llvmpipe generates code for the
  CPU it runs on, and three CI runs of identical software produced two distinct
  frames 5 634 pixels apart at a single level. A tolerance of 1 would absorb that
  and also absorb a 1% change to the AO exponent, so it would pass a genuinely
  changed renderer. CONTRIBUTING requires the gate locally for renderer, shader
  and HUD work; ROADMAP section 3 records what would close the CI gap.
- **`SceneMeasure`** exposes the world-space extent of what a node actually
  draws — the mesh's own bounds scaled by its transform — and returns null rather
  than an empty box when a node draws nothing, so a harness reading `.size.y`
  fails loudly instead of reading a plausible zero.

### User interface

- **A user-agent stylesheet under every engine document.** RmlUi ships no default
  stylesheet and RCSS defaults `display` to inline, so a `<div>` that did not
  declare its display was an inline box that silently dropped width, height,
  padding and text-align — markup written the way HTML is normally written
  collapsed into a left-aligned run of text with nothing logged. The baseline is
  embedded rather than shipped as a file so it cannot go missing from a package,
  sets `display` and nothing else, and is merged *under* the document so an
  author can still opt out.
- **`saida_tool render-ui`** renders a document without a GPU, writing the frame
  as a PNG and, with `--layout-json`, what the layout engine actually computed
  per element plus every diagnostic RmlUi raised — assertable and diffable, which
  an image is not. It is fail-closed on purpose: RmlUi renders whatever survived a
  malformed document, which would report a broken document as a successful
  render, so any load diagnostic now rejects it (`--allow-warnings` opts out).
- **`saida_tool validate-ui`** makes four silent defects mechanical. The worst:
  `rgba()` written with a 0-1 alpha parses as nothing and the *whole declaration*
  vanishes with no diagnostic at all — reported with file:line and the 0-255 value
  that was meant. The other three are rejected declarations, unresolved assets,
  and an element computing to `display:inline` while carrying width, height or its
  own text-align.
- **A screen-space canvas has a resolution to be authored at.** A canvas filling
  the viewport was resized to the real window while the scene kept declaring
  1920x1080, so absolute pixel geometry landed off-centre on every other window
  with anything past the window height silently clipped — a menu looked right only
  on the machine it was written on. `WebCanvasNode` now carries
  `referenceWidth`/`referenceHeight` and a `scaleMode` (Stretch, the default, plus
  Fit and Expand). The policy lives on the node because rendering and pointer
  input both read placement through it: a rig that drew letterboxed while
  hit-testing full-screen would put every click in the wrong place.
- **A tiled decorator tiles.** The CPU backend clamped texture coordinates past
  [0,1] instead of wrapping, so a tiled surface drew one copy and stretched its
  edge texels across the rest — plausible enough to read as a gradient rather than
  a defect. Wrapping is decided once per compiled geometry.

### Rendering

- **Bloom no longer fringes the editor viewport.** `bloom_downsample` mapped its
  centre into the rendered rectangle but then moved a texel away for each of its
  four taps with nothing bringing them back, so on the viewport edge those taps
  read the undefined area behind the editor panels — GPU-dependent magenta, cyan
  and green fringes. The clamp now covers the complete linear-filter footprint.
- **`TonemapPass` is extracted** from `Renderer.cpp` (2020 to 1894 lines), the
  first unit of the ROADMAP section 3 decomposition, with the local golden-image
  gate proving the frame did not change.

### Content

- **Sunset Strip**, an open-world slice exercising physics, traffic and scenario
  the way the vertical slice exercises the renderer: 400 m square, 568 road tiles
  across 42 blocks and 56 junctions, three districts and a beach. The layout is
  searched for rather than typed — each tile is described by the edges a road
  continues through, read out of the meshes by rasterising their kerbs, and a
  wrong reading fails loudly instead of quietly laying roads that do not join up.

### Known limitations

This is a beta, not a release candidate. The editor is still a developer
artifact rather than a portable distribution: it resolves shaders, fonts and
assets through configure-time absolute paths, and its Open Project dialog scans
the engine checkout, so a copied editor lists no projects. Windows installers
remain unsigned. ROADMAP section 1 tracks the distribution work, and sections 5
and 6 the known engine defects — among them a rejected `asset_registry.json`
being overwritten by a fresh scan, and a scene node of unknown type dropping its
whole subtree while the load still reports success.

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
