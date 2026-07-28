# Verdance — SaidaEngine vertical slice

A playable third-person slice built to exercise the engine end to end: HDRI
skybox with SH-based image-based lighting, DDGI, SSAO, bloom, distance fog,
cascaded shadows, skinned characters with clip blending, SaidaFX particles,
physics bodies and areas, UI, audio and QuickJS gameplay.

The project opens on an animated RmlUi main menu authored as separate HTML,
CSS and JavaScript files under `ui/`. Its live 3D backdrop is generated
alongside the playable scene.

## Run it

```sh
./build/bin/SaidaEngine.exe --project VerticalSlice --play
```

Or export a standalone build:

```sh
./build/bin/saida_tool.exe export-game VerticalSlice/VerticalSlice.saidaproj --platform windows --out build/verdance
```

**Controls** — `ZQSD`/`WASD` move, mouse looks, `Space` jumps (twice),
left click fires, `Shift` sprints, `R` restarts.

**The run** — shoot the three targets in the clearing, cross the bridge and the
floating islands collecting coins, then clear the guardians in the ruin and take
the star.

## Layout

| Path | What it is |
| --- | --- |
| `gen_slice.py` | Generates the menu and gameplay scenes plus the `.saidaproj`. **The scenes' source of truth** — edit this, not the generated scenes. |
| `ui/` | Main-menu structure, styling and document behavior (`main_menu.html`, `main_menu.css`, `main_menu.js`). |
| `scripts/` | Gameplay (QuickJS). `game_state.mjs` is the autoload that owns score, health, phase and the HUD; everything else talks to it through `NodeRef.call`. |
| `tools/` | The asset pipeline. Each script is idempotent and re-runnable. |
| `assets/` | Art, audio and the sky, after the pipeline has run. |

Regenerate the level with:

```sh
python VerticalSlice/gen_slice.py
```

Placement is seeded, so the same script always produces the same level.

## Verification

Two headless harnesses, both run through the editor's Play mode:

```sh
SaidaEngine.exe --project VerticalSlice --play --test-autoload "E2E=scripts/e2e_driver.js"
SaidaEngine.exe --project VerticalSlice --play --test-autoload "E2E=scripts/e2e_gameplay.js"
```

`e2e_driver.js` asserts what the scene declares (player, camera, physics, the
enemy/pickup/target/platform/pool counts, the autoload, the HUD) and
deliberately does not fire. `e2e_gameplay.js` plays the opening and asserts the
loop actually closes: a shot reaches a target, the target reports itself, the
score moves and the HUD shows it. Both print `PASS` and must leave zero
`[error]`/`[warn]` lines.

## Assets

Everything is **CC0** (public domain, no attribution required), and nothing here
was authored by hand:

| Source | Used for |
| --- | --- |
| [kenney.nl](https://kenney.nl) — Nature Kit, Platformer Kit, Blaster Kit, Cube Pets, Survival Kit, Mini Forest | environment, props, weapons, fauna |
| [kenney.nl](https://kenney.nl) — Animated Characters (Protagonists / Survivors / Retro) | the rig, its clips and the skins |
| [kenney.nl](https://kenney.nl) — Impact, Sci-Fi, Interface, RPG and Music Jingles audio | every sound |
| [polyhaven.com](https://polyhaven.com) — *Kloofendal 48d Partly Cloudy (Pure Sky)* | the 4K HDRI sky and its IBL |

### Pipeline

The kits do not drop straight into the engine. Four repairs, in order:

```sh
python VerticalSlice/tools/fix_gltf_scene_roots.py    VerticalSlice/assets/models/nature VerticalSlice/assets/models/characters
python VerticalSlice/tools/linearize_gltf_factors.py  VerticalSlice/assets/models/nature
python VerticalSlice/tools/retint_nature_palette.py   VerticalSlice/assets/models/nature
blender -b --python VerticalSlice/tools/convert_kenney_character.py -- <model.fbx> <animDir> <skin.png> <out.glb>
```

1. **`fix_gltf_scene_roots.py`** — the nature kit's exporter lists a node that
   already has a parent as the scene root. That is invalid glTF; cgltf rejects
   the file outright, so all 329 models failed to load.
2. **`linearize_gltf_factors.py`** — the kits write the artist's sRGB hex into
   `baseColorFactor`, which the spec defines as linear. Read correctly, the whole
   kit renders pastel.
3. **`retint_nature_palette.py`** — an art decision, not a fix: Kenney's nature
   palette is genuinely mint and ice blue, and this slice wanted a lush forest.
4. **`convert_kenney_character.py`** — merges the rig and its separate animation
   files into one GLB, because the engine's `Animator` only sees the clips inside
   the file a node imports. The animation files do not share the model's rest
   pose, so the clips are retargeted through armature space rather than copied as
   actions; and the FBX materials import with Alpha 0, which would export as a
   mesh that is cut out of the colour pass while still casting a shadow.

The `.obj`-based kits (platformer, blaster, pets, survival, forest) need none of
this: they share one `colormap.png` atlas per kit, so they instance cheaply as
`MeshNode` + texture. The nature kit carries its colours in glTF materials and is
imported per instance with `importedFrom`.
