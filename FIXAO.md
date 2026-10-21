# SSAO repair handoff

## Diagnosis (2026-10-21)

Lighting was unstable when the camera moved in SaidaEngine PC games, much more
so in VR over Meta Link, with the two eyes disagreeing. **With only SSAO
disabled, VRDemo's lighting was perfect** (`DiagnoseLighting.ps1 -Mode NoAO`).
VRDemo already has GI and IBL off. Do not change shadows, GI or IBL without new
evidence.

The user forbids opening windows while they work: do not launch the editor,
game, headset session or a rendering window. Compiling, CPU checks and
generating diagnostic scenes are allowed. The user runs headset tests.

## Done (2026-10-21)

SSAO was rewritten; SPEC 4.1 describes the result and CHANGELOG the defects.

- One owner: `shaders/ssao.glsl`, with `depth_reconstruction.glsl` shared with
  fog. `tonemap.frag` only calls `ssaoVisibility`. The dead `kMinAoPower` in
  `Renderer.cpp` is gone; the used clamp stays in `TonemapPass.cpp`.
- Samples are rebuilt at the centre of the texel their depth comes from.
- Normals take, per screen axis, the side whose two texels continue the
  centre's plane, else the smaller depth step; degenerate tangents fall back to
  the view direction instead of NaN.
- Every sample weighs 1/16; a rejected one counts as unoccluded.
- `saida_ssao_tests` compiles `ssao.glsl` as C++ over GLM and checks it on
  ray-cast depth buffers. Reinjecting each previous defect (and each
  intermediate normal rule tried) makes at least one of its checks fail.

Known limit: a face under three texels thick on a screen axis, at a crease,
can still take a neighbour's normal (a 5-tap estimate cannot tell the two
sides apart there). The AO factor still multiplies the whole HDR colour,
direct light included, as before.

## Remaining: headset comparison

Rebuilt and verified without a window: `build-rel` (all targets), `Shaders`,
`WebShaders`, and CTest `saida_ssao_tests`, `saida_xr_projection_tests`,
`saida_xr_rig_pose_tests`, `saida_reversed_depth_tests`,
`saida_scene_settings_tests`, `saida_lens_flare_tests`. Not run: the Witness
golden image (it renders; the AO change is expected to move it and needs a
deliberate `--record` after looking at the image).

The user compares, in the headset, from `engine/VRDemo`:

```powershell
.\PlayVR.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\DiagnoseLighting.ps1 -Mode NoAO
```

AO enabled should now be as stable as the NoAO baseline, apart from the
intended darkening in creases and contacts. A passing build or CPU test does
not establish the headset result; delete this file once it is confirmed and
close the ROADMAP XR item's lighting part.
