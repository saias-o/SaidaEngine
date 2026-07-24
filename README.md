# SaidaEngine

SaidaEngine is Saida's 3D engine. It is written in C++17, uses Vulkan on
desktop, WebGPU via WebAssembly in the browser and OpenXR for VR/AR. The
project aims for a lightweight engine that is readable and drivable by humans
as well as by AI assistants, without multiplying gameplay implementations.

**Status as of 2026-07-24: V1.0 closed on the engine side — every technical
gate is closed and the V1 refactor is complete. NO-GO for publication until the
installer is Authenticode-signed with the publishing key.**

## Canonical documents

The repository has only three documents, including this README:

- [SPEC.md](SPEC.md): architecture, public contracts, formats, subsystems,
  platforms, technical procedures, support/promotion/retirement of a release
  and current limits — the truth of what *exists*;
- [ROADMAP.md](ROADMAP.md): the single backlog of what *remains to be done* —
  the last step before publication, the code's structural debt, the deferred
  decomposition of the Renderer and its prerequisites, P1/P2 and closed
  decisions.

The web platform, backend and operations live in
[`saias-o/saida`](https://github.com/saias-o/saida). Its `PLAN_V1.md` carries
the global production go/no-go.

## Principles

- A single scene and gameplay model for editor, desktop, Web and XR.
- A single JavaScript runtime: QuickJS. RmlUi is the HTML/CSS UI system.
- A single authoring contract: SaidaOps validated into versioned snapshots.
- RAII, explicit ownership, simple components and no heavy abstraction without
  a measured need.
- Every missing capability is announced and fails explicitly. No durable
  content must be silently degraded.
- Public formats migrate; regenerable caches are not guaranteed.

## Repository

```text
src/
  authoring/   manifest, snapshots, SaidaOps
  core/        window, input, time, paths, capabilities
  editor/      Hub and ImGui editor
  graphics/    GPU resources, meshes, materials
  physics/     Jolt integration
  render/      renderer, GI, post-process, features
  runtime/     standalone desktop player
  scene/       scene, nodes, behaviours, animation
  scripting/   QuickJS and gameplay bindings
  ui/          RmlUi, WebCanvas and interaction
  xr/          OpenXR and SaidaXRTK
web/
  authoring/   headless WASM validator/fold for Saida
  player/      WASM/WebGPU game player
  runtime/     WASM/WebGPU authoring runtime
WitnessGame/   V1 witness game
tests/         native tests and frozen corpus of the V1 formats
tools/         export and smoke harnesses
```

`saida_engine` is the shared static library. `SaidaEngine` is the editor,
`SaidaEngineHub` the project manager, `SaidaEngineRuntime` the editor-less
desktop player and `saida_tool` the headless CLI used by CI and the platform.

`saida_tool describe-engine` also publishes `runtimeTypeMatrix`, the single V1
inventory of the native, headless, authoring WASM and Web player factories.
Each runtime checks its effective registry against this matrix before becoming
usable; the headless one currently round-trips the
`UINode`/`UICanvasNode`/`UITextNode` HUD and the V1 bodies/colliders without a
GPU. `saida_tool verify-manifest` proves, from the shipped binary, that every
type announced by the manifest belongs to this matrix and round-trips through
the headless snapshot codec.

## Windows prerequisites

Supported toolchain: **MSYS2 UCRT64, GCC, CMake and Ninja**. Do not mix MSVC,
MINGW64 and UCRT64.

```sh
pacman -Syu
pacman -S --needed \
  git git-lfs \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-glfw \
  mingw-w64-ucrt-x86_64-glm \
  mingw-w64-ucrt-x86_64-shaderc \
  mingw-w64-ucrt-x86_64-vulkan-headers \
  mingw-w64-ucrt-x86_64-vulkan-loader \
  mingw-w64-ucrt-x86_64-vulkan-validation-layers \
  mingw-w64-ucrt-x86_64-mesa \
  mingw-w64-x86_64-nsis
git lfs install
```

FreeType, QuickJS and RmlUi are pinned submodules:

```sh
git clone --recurse-submodules https://github.com/saias-o/SaidaEngine.git
cd SaidaEngine
git submodule sync --recursive
git submodule update --init --recursive
git lfs pull
```

A recent Vulkan GPU driver is required. The LunarG SDK is optional when the
MSYS2 packages above are used. Mesa provides the Lavapipe software ICD reserved
for CI proofs. The MINGW64 NSIS package is a standalone packaging tool (version
3.12 minimum): it does not change the engine's UCRT64 ABI.

## Build and verify

From an MSYS2 UCRT64 terminal:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For a qualified Windows release, configure with
`-DCMAKE_BUILD_TYPE=RelWithDebInfo`: packaging then separates the symbols and
strips the distributed copies. `Release` remains usable when no symbols are
expected, but does not close the V1 diagnostic gate. The GLSL shaders are
generated into `build/shaders`. A full build produces, among others:

```text
build/bin/SaidaEngine.exe
build/bin/SaidaEngineHub.exe
build/bin/SaidaEngineRuntime.exe
build/bin/saida_tool.exe
build/tests/*.exe
```

From PowerShell/Codex, put UCRT64 explicitly at the head of the `PATH` and keep
temporaries inside the workspace:

```powershell
New-Item -ItemType Directory -Force -Path build\tmp, build\msys_home | Out-Null
$env:PATH = 'C:\msys64\usr\bin;C:\msys64\ucrt64\bin;' + $env:PATH
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The symptoms `Cannot create temporary file`, a silent `cc1plus` or
`pylauncher: CreateProcess failed` usually indicate an incorrect `PATH`, `HOME`
or `TEMP`. For Emscripten, Python must also be visible.

## Web

Activate the emsdk environment, then build the two surfaces separately:

```sh
emcmake cmake -S web/player -B build-web-player -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-web-player

emcmake cmake -S web/authoring -B build-authoring-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-authoring-wasm
```

The visual authoring runtime uses `web/runtime`. `naga` and `glslc` must be
available to regenerate the WGSL shaders. Web builds are served over HTTP,
never via `file://`.

## Run

```sh
./build/bin/SaidaEngineHub.exe
# or
./build/bin/SaidaEngine.exe --project /path/game.saidaproj
```

The editor is an infinite-loop GUI application. In automation, use the tests,
`saida_tool` and the harnesses rather than leaving the executable open.

`./run.sh` launches a Debug build with the correct UCRT64 `PATH` and
`VK_LAYER_PATH` for the MSYS2 validation layers. Without this script, a layer
may load an incompatible `libstdc++` runtime.

The full native semantic corpus runs without a project:

```sh
./build/bin/SaidaEngine.exe --verify-runtime-contract
```

The Web player enables the same check with the URL parameter
`?verify-runtime-contract`; it publishes the count in `[CONTRACT] PASS`. Web
authoring systematically runs its snapshot corpus before publishing `ready` and
exposes the same verdict in the console.

Game bindings can be replaced at runtime from QuickJS with `input.rebindKey`,
`input.rebindMouse`, `input.rebindGamepadButton` and `input.rebindGamepadAxis`.
`input.rebindTouch` adds press/tap/swipes within a normalized canvas zone.
`input.exportProfile(name)` returns the versioned JSON profile; the game saves
it with `storage.prefs.save`, reloads it with `storage.prefs.load`, then calls
`input.applyProfile`. An invalid profile is rejected wholesale and leaves the
current bindings unchanged. On Web, `input.rumble(low, high, durationMs)` uses
the active pad's W3C `dual-rumble` and returns `false` if its browser or
controller does not expose it; `input.stopRumble()` stops the effect. Desktop
GLFW does not provide this backend.

V1 SaidaOps use `opVersion: 2` and target nodes by stable 64-bit identifier
encoded as a decimal string (`nodeId`, `parentId`, `newParentId`, `fromNodeId`,
`toNodeId`). This encoding avoids any JavaScript precision loss; node names are
not operation references.

```sh
./tools/witness_e2e.sh
./tools/witness_editor_play.sh
./tools/witness_editor_build.sh
./tools/witness_web_stage.sh
```

The editor Build harness requires running then restarting the exact artifact.
In CI, `SAIDA_WINDOW_HIDDEN=1` keeps the native window hidden and
`VK_DRIVER_FILES` pins Mesa/Lavapipe, which makes this path reproducible on a
clean Windows runner without a physical GPU.

The full P0.1 recipe builds both artifacts through the Build button path,
creates the archives, inventories each file and writes their SHA-256:

```powershell
.\tools\witness_release_candidate.ps1
```

It requires a clean Git worktree by default and produces
`build/release/witness-v1/` with `release-manifest.json`, the Windows and Web
archives, the Windows symbol bundle, `WitnessGame-Setup.exe`, its manifest and
their standalone verifiers. The ZIPs are canonical: ordinal order, timestamps
pinned to the commit, ambiguous paths/reparse points rejected and content
re-verified without extraction by `tools/verify_deterministic_zip.ps1`. Two
runs over the same bytes therefore produce the same SHA-256. The NSIS installer
is likewise byte-reproducible before signing. It installs per-user, inventories
every byte of the payload, rejects symlinks and case collisions, and its
uninstall removes only the inventoried files and the two explicitly named
regenerable runtime caches. Its Authenticode signature remains a separate
publishing operation that requires the signing key. `-AllowDirty` is reserved
for development proofs and explicitly writes `dirty: true` into the manifest. On
another Windows machine, no engine checkout, MSYS2 or SDK is required:
extract/copy this folder then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\verify_witness_windows.ps1
powershell -ExecutionPolicy Bypass -File .\verify_witness_installer.ps1 -RunWitness
powershell -ExecutionPolicy Bypass -File .\verify_witness_web.ps1 -Browser Chrome
powershell -ExecutionPolicy Bypass -File .\verify_witness_web.ps1 -Browser Edge -Port 18081
```

Both Windows proofs require only PowerShell and a working Vulkan driver. The
installer verifier checks the SHA-256, performs an isolated silent
installation, compares every file exactly, runs gameplay then restart and
requires a clean uninstall. The Web proofs require Python 3 and the indicated
browser. They automatically verify SHA-256, COOP/COEP, WASM MIME, gameplay/UI
and save+HUD after restart; no manual console reading is necessary.

The engine release manifest — the immutable identity of a bundle consumed by
the platform — is produced once the native and Web artifacts are built:

```powershell
.\tools\engine_release_manifest.ps1
```

It writes `build/release/engine/release-manifest.json`: commit, format versions
read from `saida_tool describe-engine`, and the SHA-256 of `saida_tool`, the
desktop runtime, the Web player, the authoring WASM, the authoring runtime and
each immutable fixture, as well as the exact compliance bundle. `-AllowDirty`
marks `dirty: true`. The executables must come from a `RelWithDebInfo` build;
the manifest also inventories their stripped copies, their `.dbg` files and the
DLL closure. The platform pins this manifest and replays it via
`tools/verify_engine_release.ps1`, which fails at the slightest byte, version or
inventory discrepancy.

The compliance bundle can also be produced on its own:

```powershell
.\tools\generate_release_compliance.ps1
```

It writes under `build/release/compliance/` the SPDX 2.3 SBOM, the GPL/third-
party notices, the hashed inventory of assets and models and their manifest.
Generation fails if a new `third_party` root or a new tracked asset lacks an
explicit license, provenance and distribution decision.

Windows `RelWithDebInfo` builds separate the distributable executables from
their diagnostic symbols:

```powershell
.\tools\package_release_symbols.ps1
```

The `build/release/windows-symbols/` bundle contains four stripped `.exe`
files, their `.dbg` files, a verified GNU debug link and a SHA-256 manifest tied
to the commit. `tools/verify_release_symbols.ps1` rejects any unexpected byte or
file. The same bundle contains `windows-dependencies.json`, recursive proof that
every x64 PE import is an authorized system DLL or a DLL that is actually
shipped; MinGW dynamic runtimes are forbidden. Desktop applications install
their crash reporter at the very start of the process: a fatal writes a
`.crash.log` and a `.dmp` minidump under
`%LOCALAPPDATA%\SaidaEngine\CrashReports\<product>\` (CI override:
`SAIDA_CRASH_DIR`). The log names the commit and the exact
`windows-symbols-<commit>` artifact to use.

AutoLOD builds separately:

```sh
cmake -S autolod -B build/autolod -G Ninja
cmake --build build/autolod --parallel
```

## Contribution rules

- Read [SPEC.md](SPEC.md) before modifying a contract or a format.
- Update [ROADMAP.md](ROADMAP.md) in the same change when an entry is closed or
  a new blocker is proven.
- Give each module and each class a clear responsibility. Split omniscient
  classes and files that mix several domains.
- Replace magic numbers and strings with named constants, types or
  configuration when their meaning is not intrinsic.
- Reject duplication, hidden dependencies, unjustified global state and long
  functions as permanent solutions.
- Prefer explicit, testable code over narrative comments. A code comment is
  only useful to explain an invariant, an external constraint or a
  non-obvious decision; it does not point back to the Markdown documents.
- Loaders accept only the exact current schema. A format change before
  publication replaces the V1 fixture and adapts all its producers.
- After an authoring-contract change, rebuild native, authoring WASM and Web
  player.
- Do not modify the sources vendored under `third_party` to work around a local
  toolchain problem.
- Do not declare a capability as supported without a real backend and an
  associated test.

## License

The project is under GPL-3.0. Dependencies and assets keep their licenses. The
inventory, notices and SBOM are generated and verified by CI. Assets explicitly
marked non-distributable stay out of the V1 bundles; consult the release guide
([SPEC.md](SPEC.md) §17) before any promotion.
