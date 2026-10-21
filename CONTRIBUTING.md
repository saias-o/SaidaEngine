# Contributing to SaidaEngine

Thank you for contributing to SaidaEngine. This document describes the shared
development workflow. Automated coding agents must also follow
[AGENTS.md](AGENTS.md).

## Before making a change

1. Read [SPEC.md](SPEC.md) before modifying architecture, a public contract or
   a persistent format.
2. Read [ROADMAP.md](ROADMAP.md) to understand current priorities and closed
   decisions.
3. Update `ROADMAP.md` in the same change when an entry is completed or a new
   blocker is proven.
4. Keep documentation and code comments in English.

## Working tree and generated content

- Inspect the worktree before editing and preserve unrelated changes already in
  progress.
- Keep each change focused; do not perform unrelated refactors.
- Do not weaken, delete or bypass a test merely to make a change pass.
- Do not edit vendored sources under `third_party`.
- Modify Witness generators rather than generated Witness output when the
  generator is the source of truth.
- Never regenerate a frozen fixture merely to hide a divergence. A format
  change updates its producers, loaders, fixtures and tests together.
- Do not commit build directories, generated packages, caches, crash reports,
  credentials or signing material.

## Design and implementation rules

- Give every module and class a clear responsibility.
- Split classes and files that mix unrelated domains.
- Prefer explicit ownership, RAII and testable code.
- Replace non-intrinsic magic numbers and strings with named constants, types
  or configuration.
- Do not introduce hidden dependencies, unjustified global state, duplicated
  implementations or long functions as permanent solutions.
- Add a code comment only for an invariant, external constraint or non-obvious
  decision. Do not use comments as a substitute for clear code.
- Do not announce a capability as supported without a real backend and an
  associated test.
- Missing capabilities must fail or degrade explicitly; durable content must
  never be silently discarded.

## Contracts and formats

- Loaders accept only the exact current schema.
- Before stable publication, an intentional format change updates the schema,
  every producer and the frozen corpus together.
- After an authoring-contract change, rebuild the native engine, authoring WASM
  and Web player.
- Changes to snapshots, SaidaOps, manifests, registries, scripting or shared
  input require cross-runtime verification.
- Public formats are migrated; regenerable caches are not compatibility
  contracts.

## Windows development environment

Use an MSYS2 UCRT64 terminal. Install the prerequisites listed in
[README.md](README.md), then configure and test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

From PowerShell, put UCRT64 first in `PATH` and keep temporary directories
inside the workspace:

```powershell
New-Item -ItemType Directory -Force -Path build\tmp, build\msys_home | Out-Null
$env:PATH = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Errors such as `Cannot create temporary file`, a silent `cc1plus`, or
`pylauncher: CreateProcess failed` usually mean that `PATH`, `HOME` or `TEMP`
points at an incompatible environment.

Git Bash is not an MSYS2 UCRT64 shell. Compilation can succeed there while the
link step fails with `collect2.exe: error: ld returned 116 exit status` because
`ld` found incompatible runtime DLLs. Put the UCRT64 binaries first before
building:

```sh
export PATH="/c/msys64/ucrt64/bin:$PATH"
cmake --build build --parallel
```

The same error does not indicate a source defect when the build succeeds from a
native UCRT64 shell.

## Required verification

Native games must consume the build-tree CMake SDK (`<engine-build>/sdk`) through
`Saida::Engine`/`saida_add_game`; do not parse Ninja files or duplicate compiler
flags in a game launcher. Keep gameplay sources and generated registration in
the project. `WITH_EDITOR` supplies a project editor with those same types;
never patch the engine's built-in `ReflectedTypes.cpp` to add game logic.
`templates/native-game` is the minimal independent consumer. CTest's
`saida_native_sdk_consumer` configures, builds and packages a copy in a path
containing spaces and a shell metacharacter, builds its project editor and
verifies that an incompatible SDK configuration fails explicitly. Desktop
capture/play tests remain separate from this compiler-only check.

Run checks proportional to the change and report exactly which commands were
run. The baseline verification is:

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/saida_tool.exe describe-engine
```

To see where a run spends its time, pass `--profile <trace.json>` to the player
(or to any executable that calls `Engine::profileTo`): the trace is written when
the run ends, and the log gets the most expensive scopes per frame. Measure an
optimized build: the Debug tree is several times slower and ranks scopes
differently.

The terrain-ring test executable also provides a fixed-content GPU benchmark:

```powershell
$env:SAIDA_WINDOW_HIDDEN = '1'; $env:SAIDA_WINDOW_SIZE = '1600x900'
build-rel/tests/saida_terrain_rings_tests.exe --benchmark air --profile air.json --out air.png
build-rel/tests/saida_terrain_rings_tests.exe --benchmark eye --profile eye.json --out eye.png
```

Add `--reference` to bypass whole-cell rejection in the vertex shader, retaining
the original fragment clip. The native CTest render case compares exact pixels
against that shader, including the MSAA boundary guard. Benchmark traces include
the culling pre-pass: compare total GPU frame time as well as the ring draw, and
exclude capture/readback frames when reporting steady timings. These fixtures
do not qualify a complete game's frame budget.

The native semantic contract can also be checked without a project:

```sh
./build/bin/SaidaEngine.exe --verify-runtime-contract
```

Use the dedicated Witness harnesses for end-to-end work:

```sh
./tools/witness_e2e.sh
./tools/witness_editor_play.sh
./tools/witness_editor_build.sh
./tools/witness_web_stage.sh
./tools/witness_golden_image.sh   # renderer/shader/HUD changes; needs Lavapipe
python tools/verify_material_sidedness.py --build build  # Pillow; native front/back pixel check
python tools/verify_ibl_environment.py --build build    # Pillow; HDR transfer and environment pixel checks
python tools/verify_specular_materials.py --build build # Pillow; glTF IOR/specular RGB and alpha pixel checks
python tools/verify_billboard_materials.py --build build # Pillow; atlas facing/blend and directional lighting
node tools/verify_billboard_web.mjs build/shaders/wgsl # Playwright; translated scene pipeline validation
```

The desktop and Web restart checks must restore the saved progression and reach
their explicit PASS verdict.

`witness_golden_image.sh` compares a captured frame of the WitnessGame hub scene
against a committed reference, exactly. The reference is a Lavapipe image and
the gate refuses any other backend — two rasterizers disagree on every lit
surface. The comparison carries no tolerance because one was measured letting a
changed renderer through: a 1% change to the AO exponent moves 2 175 pixels by a
single level, so `--tolerance 1` reported PASS on it.

This check is local and is not run by CI: llvmpipe generates code for the CPU it
runs on, so the same commit renders differently on different machines (measured
across GitHub's hosted runners: two distinct frames, 5 634 pixels apart at a
single level, with Mesa and LLVM identical). Record the reference on the machine
that will check it, where the capture is byte-identical run to run. A failure
where every difference is a single level means a different host — or a subtle
renderer change, which looks the same; check whether `src/render` or `shaders/`
actually moved. `--record` rewrites the reference for an intended change; look
at the new image before committing it.

For a visual defect outside the gameplay camera, capture an exported player
from an explicit viewpoint:

```sh
"./Game.exe" --screenshot look.png --after-frames 30 \
    --camera-pos 3,0.25,3 --camera-look 0,0.3,0
```

Both camera flags are required, and the two points must differ. A rejected
viewpoint exits non-zero and writes no image. Inspect the PNG itself: geometry
that is floating, sunk into the ground or scaled incorrectly can pass every
structural test while remaining obvious from a grazing angle.

Do not leave GUI applications or local verification servers running after the
check.

The environment check isolates direct light, ambient and GI. It checks HDR
greys against ACES plus one IEC sRGB transfer, then diffuse/specular crossfades,
secondary rotations and missing-secondary fallback. The directional sky case
also catches a neutral normal map accidentally sampled as sRGB. The sidedness
check uses explicit ambient illumination so display calibration cannot hide
otherwise correctly culled panels below its colour threshold.

## Authoring game UI

Start by copying `templates/ui/` into the game's `ui/` directory. `menu.html`
loads `components.css`, `menu.css` and `menu.js`; `button.html`, `list.html`
and `panel.html` are independent examples. `panel.rml` is an actual RmlUi
template (`<link type="text/template" href="panel.rml"/>`, then
`<body template="ui-panel">`). Reuse the `.ui-button`, `.ui-panel`, `.ui-field`,
`.ui-list`, `.ui-list-item`, `.ui-row` and `.ui-stack` classes instead of
rebuilding focus, hover and spacing rules in each game.

Use normal semantic containers (`main`, `nav`, `header`, `section`, `footer`)
and flex layout. An ordinary `<html>` root, `<!DOCTYPE html>`, void tags and
`<link rel="stylesheet" href="menu.css">` are accepted. External `.css` files,
HTML-root inline styles and the JS style API accept
`rgba(20, 30, 40, 0.5)` with Web alpha. Native `.rcss` and inline RML styles
retain byte alpha; `#RRGGBBAA` is unambiguous. Web-profile declarations accept
`transition: opacity 180ms ease-out` and `animation: enter 800ms ease-out 1`;
the ease aliases map to native cubic tweens. Native RCSS can use
`transition: background-color transform 0.18s cubic-out` and
`animation: 0.8s cubic-out 1 enter`. Set the underlying style to the intended
resting pose: completed CSS animations return to the underlying style.

The DOM subset includes selectors, classes, text/markup, attribute methods,
form `.value`, `.hidden`, listeners, `style.setProperty/getPropertyValue` and
common camel-case style accessors (`element.style.opacity`, `backgroundColor`,
`transform`, sizes/position, `animation`, `transition`). In a listener,
read `event.target.value` for the player's input. Use classes for animated
state changes. CSS animations run autonomously on unscaled UI time; no global
timer, browser `window`, React runtime or network access is implied.

Validate and inspect the actual renderer before declaring a UI finished:

```powershell
build-rel/bin/saida_tool.exe validate-ui templates/ui/menu.html --project . --pretty
build-rel/bin/saida_tool.exe render-ui templates/ui/menu.html --project . --size 1280x800 --time 0.5 --out build-rel/menu-half.png --layout-json build-rel/menu-half.json
build-rel/tests/saida_ui_gpu_tests.exe build-rel/menu-gpu.png
```

`render-ui` samples CSS animations at 60 Hz and renders the final state with
the CPU reference; document JavaScript is exercised in the native canvas.
The GPU test writes a real Vulkan image plus `.timings.json` with CPU raster
and GPU submit-plus-fence wall times on that machine. Those timings do not
qualify another GPU. Inspect the PNG and test the actual clicks, focus and
resize. Unsupported shader decorators and filters are diagnostics, not
successful approximations. Native document canvases use GPU rasterization;
`SAIDA_UI_RENDERER=cpu` selects the reference worker path for comparisons.

## Web verification commands

Activate the Emscripten SDK and build:

```sh
emcmake cmake -S web/player -B build-web-player -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-web-player

emcmake cmake -S web/authoring -B build-authoring-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-authoring-wasm
```

Serve Web output through HTTP(S). Do not test it through `file://`.

## Release qualification

The step-by-step editor publication procedure is [RELEASE.md](RELEASE.md). It is
the operational checklist derived from this document and SPEC section 17; use
it verbatim so the ZIP, installer, proof and GitHub release cannot drift.

Release work starts from a clean commit. The complete portable Windows editor
candidate is produced and independently exercised with:

```powershell
.\tools\editor_release_candidate.ps1
```

The separate Windows and Web Witness candidate is produced with:

```powershell
.\tools\witness_release_candidate.ps1
```

It writes the candidate under `build/release/witness-v1/`, including the
deterministic Windows and Web archives, symbol bundle, installer, manifests and
standalone verifiers.

Generate and verify the immutable engine identity with:

```powershell
.\tools\engine_release_manifest.ps1
.\tools\verify_engine_release.ps1
```

Generate compliance and diagnostic artifacts with:

```powershell
.\tools\generate_release_compliance.ps1
.\tools\package_release_symbols.ps1
```

The generated SPDX SBOM, notices, asset inventory, binary hashes and symbols
must match the release commit. A release must never be rebuilt under an
existing identity.

The external verification bundle supports:

```powershell
powershell -ExecutionPolicy Bypass -File .\verify_witness_windows.ps1
powershell -ExecutionPolicy Bypass -File .\verify_witness_installer.ps1 -RunWitness
powershell -ExecutionPolicy Bypass -File .\verify_witness_web.ps1 -Browser Chrome
powershell -ExecutionPolicy Bypass -File .\verify_witness_web.ps1 -Browser Edge -Port 18081
```

Authenticode signing is a separate publishing operation. An unsigned beta can
be used for manual testing, but it is not a qualified Windows distribution.
The public [Code signing policy](CODE_SIGNING_POLICY.md) defines the signing
roles, privacy statement and trusted-build controls.

After SignPath Foundation accepts the project, the release-signing integration
must use SignPath's GitHub trusted build system and origin verification. The
unsigned installer must first be uploaded by `actions/upload-artifact`, then
submitted with
`signpath/github-action-submit-signing-request@v2`. The organization ID,
project slug, signing-policy slug and artifact-configuration slug must come from
the provisioned SignPath project; do not invent or hard-code placeholder
identifiers.

Every GitHub release page that contains a signed Windows installer must:

- link to the [Code signing policy](CODE_SIGNING_POLICY.md);
- include the statement “Free code signing provided by SignPath.io, certificate
  by SignPath Foundation”;
- identify the immutable tag and full source commit;
- publish the SHA-256 digest of the signed installer;
- state whether manual qualification is complete.

## Versioning

The engine has exactly two version notions, each with a single home. Never
hard-code a version string anywhere else.

- **Product / release version** — the human-facing version shown in the About
  box, written into exe metadata, quoted in the docs, and used for the git
  release tag (prefixed `v`). It follows [SemVer](https://semver.org): a
  `MAJOR.MINOR.PATCH` core plus an optional pre-release suffix such as
  `-beta.1`. Its single source of truth is `kProductVersion` in
  `src/core/EngineVersion.hpp`. The current value is `1.0.0-beta.4`. Bump it in
  that one file on every release; keep `kProductVersionNumeric` (the suffix-free
  core, used for Windows `VERSIONINFO`) in sync.
- **Engine format / contract version** — `kEngineVersion` in the same file
  (`1.0.0`). It is recorded in every project file and the authoring manifest and
  governs on-disk document migration (see [SPEC.md](SPEC.md)). It is a data
  contract, not a marketing version: change it only when the persisted format
  changes, and never fold a pre-release suffix into it.

## Beta and release-candidate policy

- Each manual test and correction cycle receives a new beta:
  `v1.0.0-beta.1`, `v1.0.0-beta.2`, and so on.
- Never move or rebuild an existing beta tag.
- A release candidate is created only when the owner considers the beta test
  cycle satisfactory.
- Fixes found during an RC require a new immutable RC.
- Stable `v1.0.0` is published only after the final qualification is accepted.

## Pull requests

- Include tests proportional to the changed contract.
- Describe known limitations and unverified surfaces honestly.

## License and assets

The built-in XR hand models come from the MIT-licensed Godot OpenXR demo.
`python tools/import_xr_hands.py` fetches its pinned upstream revision and packs
the glTF buffers and texture into standalone `assets/xr/hands/*.glb`, retaining
the original rig and mesh. It also saves the upstream MIT notice. Python is
needed only to reimport these assets, never to build or run the engine. Run
`saida_xr_hand_tests` after changing the models or tracking-to-rig mapping.

All contributions are distributed under GPL-3.0. New dependencies and assets
must have explicit license, provenance and distribution entries. Compliance
generation intentionally fails when an entry is missing.

Binary assets are tracked through Git LFS, not ignored. The repository's
`.gitattributes` routes formats including GLB, glTF, PNG, FBX and TGA through
LFS. Commit such files through LFS; if an asset must stay outside the repository,
leave it untracked instead of changing the repository-wide ignore or LFS policy
without an explicit decision.
