# SaidaEngine release procedure

This is the executable publication runbook for humans and LLM agents. It
implements the release contract in [SPEC.md](SPEC.md) and
[CONTRIBUTING.md](CONTRIBUTING.md). Do not substitute remembered steps, do not
reuse an old proof, and do not modify an existing tag or release.

The current target is `v1.0.0-beta.4`. `v1.0.0-beta.3` is immutable and must not
be edited, retagged or have its assets replaced.

## 1. Supported Windows package

The Beta 4 Windows package supports Windows 11 x64. It contains the four engine
executables, GLFW, the Vulkan loader, shaders, editor assets and fonts, a sample
project, licenses and compliance records. A developer does **not** install
Visual C++ Redistributable, Vulkan SDK/runtime, MSYS2, CMake, Python or a
compiler to launch it or export the included sample.

The machine must have a normal Intel, AMD or NVIDIA graphics driver exposing
Vulkan 1.3. That is a hardware/driver baseline, not a separate engine runtime.
No software can promise Vulkan functionality on unsupported graphics hardware;
never hide this limit in release notes.

The portable ZIP and installer are two containers for the same inventoried
payload. They are not qualified merely because compilation succeeded. The ZIP
must run after extraction and the installer must install, run and uninstall on
a Windows 11 machine without a SaidaEngine checkout or development toolchain.

## 2. Non-negotiable publication rules

1. Release only a clean, committed `main` HEAD. `-AllowDirty` exists solely for
   local pipeline development and its output must never be uploaded.
2. `src/core/EngineVersion.hpp`, the changelog heading and the new tag must
   identify the same version. A version is never inferred from an old tag.
3. Wait for every required GitHub Actions job on that exact commit to be green.
   A green Windows job does not override a failed Web or Linux job.
4. Create a new immutable tag for any changed byte. Never move a tag, replace
   an asset in an existing release or rebuild under an existing version.
5. ZIP, installer, manifests, hashes and symbols must come from one invocation
   of `tools/editor_release_candidate.ps1`.
6. Unsigned beta installers must be described as unsigned. Stable publication
   remains blocked until the Authenticode policy in
   [CODE_SIGNING_POLICY.md](CODE_SIGNING_POLICY.md) is satisfied.

## 3. Prepare the release commit

From a recursive Git LFS checkout:

```powershell
git switch main
git pull --ff-only
git submodule sync --recursive
git submodule update --init --recursive
git lfs pull
git status --short
```

`git status --short` must print nothing. For a new beta, change only the release
version source, add a matching `CHANGELOG.md` section, review the documented
requirements and commit those changes. Re-run the clean-status check after the
commit. Record the full commit:

```powershell
$releaseCommit = (git rev-parse HEAD).Trim()
$releaseVersion = (Select-String src/core/EngineVersion.hpp `
  'kProductVersion\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value
```

Stop if `$releaseVersion` is not the intended new version.

## 4. Produce and verify the Windows candidate

The build host uses the repository's documented UCRT64 environment. Run the
single recipe from the repository root:

```powershell
.\tools\editor_release_candidate.ps1
```

The script fails unless the worktree is clean, the built product version equals
the source version, all native tests pass, all DLL imports close, compliance is
complete, and both publishable formats execute successfully. It writes
`build/release/editor-v1/` containing:

- `SaidaEngine-v<VERSION>-windows-x64.zip` — portable product;
- `SaidaEngine-v<VERSION>-windows-x64-Setup.exe` — per-user installer;
- the installer payload manifest and `release-manifest.json`;
- `SHA256SUMS.txt`;
- `windows-symbols/` and its verifier;
- standalone ZIP and installer verification scripts.

The generated `release-manifest.json` must say `dirty: false`, contain
`$releaseCommit`, identify the intended version and state
`additionalRuntimeInstall: false`. The dependency report must classify locally
shipped `glfw3.dll` and `vulkan-1.dll` as bundled and contain no missing DLL.

## 5. Independent Windows 11 acceptance

Copy the complete `build/release/editor-v1/` directory to a fresh Windows 11
x64 user account or VM that has a Vulkan 1.3-capable graphics driver, but no
SaidaEngine checkout, MSYS2, Visual Studio, Vulkan SDK, CMake or compiler.

Run both proofs in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\verify_editor_zip.ps1 `
  -ManifestPath .\release-manifest.json
powershell -ExecutionPolicy Bypass -File .\verify_editor_installer.ps1 `
  -ManifestPath .\SaidaEngine-v1.0.0-beta.4-windows-x64-Setup.manifest.json
```

Both must print `VERIFY PASS`. These checks use only the extracted or installed
product. They start the Hub/editor/CLI, export the included WitnessGame, run it,
verify its persisted restart, compare the installer payload byte-for-byte and
verify clean uninstall. Save the console logs with the release evidence.

Do not call the Windows package qualified when this exact fresh-machine proof
has not been run. CI provides an automated isolated-layout proof on a hosted
Windows runner; because that runner contains its build toolchain, it does not
replace the final manual run on the clean VM.

## 6. Tag and publish on GitHub

Only after the exact commit's CI and the Windows acceptance above pass:

```powershell
git tag -a v1.0.0-beta.4 $releaseCommit -m "SaidaEngine 1.0.0 Beta 4"
git push origin v1.0.0-beta.4
```

Create a new GitHub release for that tag and mark it as a **pre-release**. Use
the matching changelog section as the release body, then add:

- full source commit;
- Windows 11 x64 and Vulkan 1.3 driver requirement;
- explicit statement that no VC++ redistributable, Vulkan SDK/runtime or build
  tool installation is required;
- Authenticode status (`unsigned beta` until signing is provisioned);
- the contents of `SHA256SUMS.txt`;
- fresh-machine qualification result and date.

Upload, without renaming or rebuilding:

1. the Windows ZIP;
2. the Setup executable;
3. Setup manifest;
4. `release-manifest.json`;
5. `SHA256SUMS.txt`;
6. standalone verification scripts;
7. the symbol directory as a separate archive if public crash diagnosis is
   desired.

Before pressing **Publish release**, download the draft assets once, recompute
their SHA-256 values and compare them to `SHA256SUMS.txt`. Confirm the tag still
resolves to `$releaseCommit`. Publication is a deliberate human confirmation;
an LLM must stop and request authorization before creating or publishing the
GitHub release when that authorization was not explicitly given.

## 7. Failure handling

Any failed command invalidates the candidate. Fix forward, commit, wait for CI
and rebuild the whole candidate. Never patch files inside the ZIP, installer or
release draft. If a defect is found after publication, leave Beta 4 immutable,
mark its limitation publicly and publish Beta 5 from a new commit and tag.
