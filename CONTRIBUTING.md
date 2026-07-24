# Contributing to SaidaEngine

Thank you for contributing to SaidaEngine. This document describes the human
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
- Do not edit vendored sources under `third_party` to work around a local
  toolchain problem.
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
$env:PATH = 'C:\msys64\usr\bin;C:\msys64\ucrt64\bin;' + $env:PATH
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Errors such as `Cannot create temporary file`, a silent `cc1plus`, or
`pylauncher: CreateProcess failed` usually mean that `PATH`, `HOME` or `TEMP`
points at an incompatible environment.

## Required verification

The baseline verification is:

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
```

The desktop and Web restart checks must restore the saved progression and reach
their explicit PASS verdict.

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

Release work starts from a clean commit. The complete Windows and Web candidate
is produced with:

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
  `src/core/EngineVersion.hpp`. The current value is `1.0.0-beta.1`. Bump it in
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

- Keep each change focused.
- Include tests proportional to the changed contract.
- Describe known limitations and unverified surfaces honestly.
- Do not mix generated build output with source changes.
- Do not commit credentials, signing keys, local caches or crash dumps.

## License and assets

All contributions are distributed under GPL-3.0. New dependencies and assets
must have explicit license, provenance and distribution entries. Compliance
generation intentionally fails when an entry is missing.
