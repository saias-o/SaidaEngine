# SSAO repair handoff

## Confirmed diagnosis (2026-10-21)

The user reports unstable lighting when moving the camera in SaidaEngine PC
games, much more pronounced in VR over Meta Link, with binocular disagreement.
This is specific to SaidaEngine, not games using other engines.

**The user tested VRDemo with only SSAO disabled and reported that the lighting
was perfect, with no remaining problem.** This isolates the reported artifact
to the screen-space ambient occlusion pass. VRDemo already has GI and IBL off.
Do not keep changing shadows, GI or IBL without new evidence.

The user explicitly forbids opening windows while they work. Do not launch the
editor, game, headset session or a rendering window automatically. Compilation,
CPU checks and generating diagnostic scenes are allowed. The user performs
headset tests themselves.

## Current code and unsuccessful symptom fixes

- `shaders/tonemap.frag` currently owns SSAO: 8 directions x 2 samples,
  reconstruction of normals from the centre/right/down depth samples, and
  multiplication of the entire HDR colour by the AO factor.
- `src/render/TonemapPass.cpp/.hpp` pack AO and inverse-projection parameters.
- Previous changes preserve asymmetric OpenXR projection offsets, scale the AO
  sample radius by projection, and use per-eye positions for mesh specular,
  parallax and view-biased GI. These are concrete corrections, but **the user
  confirmed they did not resolve the reported instability**.
- `tests/XrProjectionTests.cpp` checks frustum edges, reversed depth, a world
  plane reconstructed through both eyes under head motion, and desktop
  perspective/orthographic reconstruction. All 52 checks passed. These prove
  projection math, not the visual quality/stability of SSAO.
- Native shaders and `WebShaders` compiled successfully. The 5 targeted tests
  (`xr_projection`, `xr_rig_pose`, `reversed_depth`, `scene_settings`,
  `lens_flare`) passed without a window.

## Diagnostic launcher

The engine demo is **`VRDemo/` inside the engine repository**, not a sibling of
the engine. It is a normal `.saidaproj` plus an editable `scenes/main.scene`.
`PlayVR.cmd` launches `../build-rel/bin/SaidaEngine.exe` with `--xr`.

From `engine/VRDemo`, the confirmed successful comparison was:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\DiagnoseLighting.ps1 -Mode NoAO
```

Other available modes are `NoShadows` and `NoAOOrShadows`. Add `-PrepareOnly`
to generate a variant without launching anything. Variants are written under
ignored `VRDemo/build/lighting-diagnostics/`; the authored scene is unchanged.
Structural comparisons verified that NoAO changes only `settings.aoEnabled`,
and NoShadows changes only the casting light's `castShadows` field.

## Next work

The user's latest technical request is to clean up the AO implementation,
which they consider tangled and ineffective. Do not add more speculative
renderer patches or simply present disabling AO as an implementation fix.

1. Audit and simplify the shared PC/VR SSAO implementation. Give it one clear
   owner, ideally a small shader include, rather than mixing all AO logic into
   tonemapping. Keep depth reconstruction shared with fog where appropriate.
   Remove dead remnants such as the unused `kMinAoPower` in `Renderer.cpp`;
   the used clamp belongs in `TonemapPass.cpp`.
2. Fix depth-edge normal estimation. Current right/down-only finite differences
   can combine two different objects, or a surface and background. A simple
   numerical example gives a normal error of 89.98 degrees for a flat surface
   next to a distant depth sample. This can make lighting patches move with
   silhouettes. Use valid neighbours from both sides of each axis and prefer
   the smaller depth discontinuity; guard degenerate cross products before
   normalization. This proposed change has **not yet been implemented**.
3. Review occlusion normalization. Currently `samples` increases only after
   depth/distance rejection. A few surviving samples can therefore amplify
   darkening abruptly as the camera moves. A fixed attempted-sample weight
   with rejected samples treated as unoccluded is a candidate, to be tested,
   not a verified fix. Keep radius/distance weights continuous.
4. Add meaningful no-window tests for flat planes, depth edges, background
   neighbours, degenerate normals, camera motion and left/right consistency.
   Prefer testing actual shared shader math over copying an implementation
   into a test. One possible approach is to keep the small GLSL math include
   within a C++-compatible subset and compile it in the CPU test using GLM.
   No new AO refactor or test has been written yet.
5. Rebuild native shaders, WebShaders and the binaries the user actually runs.
   Run proportional headless checks. Retest with AO enabled on the same scene,
   and ask the user to compare against the confirmed NoAO baseline. A passing
   build or projection test does not establish that the visual problem is fixed.

## Build and repository notes

Follow `AGENTS.md`, `CONTRIBUTING.md`, `SPEC.md` and `ROADMAP.md`. Keep durable
engine contracts in SPEC, open work in ROADMAP, and demo-specific instructions
in `VRDemo/README.md`. This handoff is temporary, not a competing specification.

Use the existing `build-rel` MSYS2 UCRT64 configuration; put
`C:\msys64\ucrt64\bin;C:\msys64\usr\bin` first on PATH. Mixing MINGW64 DLLs can
cause a silent `ld returned 116` unrelated to the change. The parent R1World
repository tracks this engine as a Git submodule; commit/push engine changes
before updating and pushing the parent's submodule reference.
