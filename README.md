# SaidaEngine

SaidaEngine is Saida's local-first 3D engine. It is written in
C++17 and targets:

- Windows desktop through Vulkan;
- the browser through WebAssembly and WebGPU;
- VR/AR through OpenXR;
- headless validation and scene folding through `saida_tool`.

The editor, desktop runtime, Web player and authoring tools share one scene and
gameplay model. QuickJS is the scripting runtime, RmlUi is the HTML/CSS UI
system, and durable edits use validated SaidaOps and versioned snapshots.

## Project status

The current public line is **1.0.0 Beta**. Beta releases are intended for manual
engine testing and feedback. They are not stable releases and may contain
blocking defects. Each round of fixes receives a new beta; release candidates
start only after the manual qualification is satisfactory.

The Windows installer is not an officially qualified distribution until its
Authenticode signature has been verified. GitHub beta releases may therefore
contain source code and CI-produced artifacts without representing a signed
production release.

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate
by [SignPath Foundation](https://signpath.org/).

The signing roles, trusted-build requirements, release controls and privacy
statement are defined in the
[Code signing policy](CODE_SIGNING_POLICY.md). SaidaEngine does not include
telemetry or automatically upload crash reports. Releases and CI artifacts state
explicitly when a Windows installer is unsigned.

## Documentation

The repository is governed by five documents:

| Document | Audience | Purpose |
|---|---|---|
| [SPEC.md](SPEC.md) | Everyone | Canonical architecture, contracts, formats, supported surfaces and technical limits |
| [ROADMAP.md](ROADMAP.md) | Everyone | Remaining work, priorities, blockers and closed decisions |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Contributors | Development setup, contribution rules, tests and release verification |
| [AGENTS.md](AGENTS.md) | LLM agents | Mandatory instructions for automated coding agents |
| [LICENSE](LICENSE) | Everyone | GPL-3.0 license |

The Web platform, backend and operations live in
[`saias-o/saida`](https://github.com/saias-o/saida).

## Download

### Beta release

Download the latest beta from
[GitHub Releases](https://github.com/saias-o/SaidaEngine/releases). Every beta
is a pre-release and is identified by an immutable tag such as
`v1.0.0-beta.2`.

When a release does not provide a signed Windows installer, use the source
archive or build the engine locally. Do not treat the mutable `latest`
container tag as a release identity. The
[Code signing policy](CODE_SIGNING_POLICY.md) explains how official Windows
installers are built, approved, signed and identified.

### Source checkout

Git LFS and the recursive submodules are required:

```sh
git lfs install
git clone --recurse-submodules https://github.com/saias-o/SaidaEngine.git
cd SaidaEngine
git submodule sync --recursive
git submodule update --init --recursive
git lfs pull
```

To check out a specific beta:

```sh
git checkout v1.0.0-beta.2
git submodule update --init --recursive
git lfs pull
```

## Repository layout

```text
src/
  authoring/   manifests, snapshots and SaidaOps
  core/        window, input, time, paths and capabilities
  editor/      Hub and editor
  graphics/    GPU resources, meshes and materials
  physics/     Jolt integration
  render/      renderer, GI, post-processing and features
  runtime/     standalone desktop player
  scene/       scene, nodes, behaviours and animation
  scripting/   QuickJS and gameplay bindings
  ui/          RmlUi, WebCanvas and interaction
  xr/          OpenXR and SaidaXRTK
web/
  authoring/   headless WASM validation and fold
  player/      WASM/WebGPU game player
  runtime/     WASM/WebGPU authoring runtime
WitnessGame/   vertical test game
tests/         native tests and frozen format corpus
tools/         export, packaging and verification tools
plugins/       optional packages that are not built or loaded by the engine
```

The principal targets are:

- `saida_engine`: shared static engine library;
- `SaidaEngine`: editor;
- `SaidaEngineHub`: project manager;
- `SaidaEngineRuntime`: editor-less desktop player;
- `saida_tool`: headless CLI used by CI and the Saida platform.

The repository also contains
[`saida-python-tools`](plugins/python-tools/README.md), an optional standalone
Python authoring SDK for project generators, converters and audits. It is not a
CMake target, is never loaded by the editor or runtime, and is not included in
game exports. Projects opt in with a separate `saida.tools.toml`; installing
Python is not required to build, run or ship SaidaEngine. Coding agents should
start with its
[Python scripting guide](plugins/python-tools/LLM_SCRIPT_GUIDE.md).

## Windows prerequisites

The supported Windows toolchain is **MSYS2 UCRT64 with GCC, CMake and Ninja**.
Do not mix MSVC, MINGW64 and UCRT64 binaries.

From an MSYS2 UCRT64 terminal:

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
```

A recent Vulkan GPU driver is required. The LunarG SDK is optional when the
MSYS2 packages are installed. FreeType, QuickJS and RmlUi are pinned
submodules.

## Build

From an MSYS2 UCRT64 terminal:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For a qualified Windows build, use `RelWithDebInfo`:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The main outputs are:

```text
build/bin/SaidaEngine.exe
build/bin/SaidaEngineHub.exe
build/bin/SaidaEngineRuntime.exe
build/bin/saida_tool.exe
build/tests/*.exe
```

More complete verification and packaging procedures are documented in
[CONTRIBUTING.md](CONTRIBUTING.md).

## Run

```sh
./build/bin/SaidaEngineHub.exe
# or
./build/bin/SaidaEngine.exe --project /path/to/game.saidaproj
```

`./run.sh` launches a Debug build with the correct UCRT64 paths and Vulkan
validation-layer setup.

The editor is a GUI application with an event loop. Automation should use
CTest, `saida_tool` and the dedicated harnesses instead of leaving the editor
running.

## Web builds

Activate the Emscripten SDK environment, then build the player and authoring
surfaces separately:

```sh
emcmake cmake -S web/player -B build-web-player -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-web-player

emcmake cmake -S web/authoring -B build-authoring-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-authoring-wasm
```

Web builds must be served through HTTP(S), never opened with `file://`. WebGPU
must be enabled in a supported browser.

## Container

The public headless image is:

```sh
docker pull ghcr.io/saias-o/saida-tool:latest
```

For reproducible deployments, pin a commit tag or immutable digest instead of
`latest`.

## License

SaidaEngine is distributed under GPL-3.0. Dependencies and assets retain their
own licenses. CI generates and verifies the SPDX SBOM, third-party notices and
the asset/model inventory.
