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

Run checks proportional to the change and report exactly which commands were
run. The baseline verification is:

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/saida_tool.exe describe-engine
```

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

## Web verification

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

All contributions are distributed under GPL-3.0. New dependencies and assets
must have explicit license, provenance and distribution entries. Compliance
generation intentionally fails when an entry is missing.

Binary assets are tracked through Git LFS, not ignored. The repository's
`.gitattributes` routes formats including GLB, glTF, PNG, FBX and TGA through
LFS. Commit such files through LFS; if an asset must stay outside the repository,
leave it untracked instead of changing the repository-wide ignore or LFS policy
without an explicit decision.
