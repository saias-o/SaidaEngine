# VRDemo

A standard, data-only SaidaEngine project: `VRDemo.saidaproj` selects
`scenes/main.scene`. Open the project in the engine Hub or use `OpenEditor.cmd`.
The scene stays editable in the normal scene tree and Inspector; no custom game
types, native registration or generated runtime scene are required.

## Try it

Enable the headset's OpenXR runtime and optical hand tracking, then launch
`PlayVR.cmd`. On Quest through Link, use the Meta/Oculus OpenXR runtime and make
sure hand tracking is available to the PC session.

Stand at the floor marker, facing the table. Bring either hand near the orange
cube, close your hand to grab it, move it, then open your hand to release it.
Squeezing a tracked controller's grip also works. The cube uses the engine's
`XRGrabbable`, including its physical release/throw behavior. Restart the scene
to put it back on the table.

The launch command uses the engine's normal isolated XR preview:

```powershell
../build-rel/bin/SaidaEngine.exe --project VRDemo.saidaproj --xr --scene scenes/main.scene
```

`XROrigin` stays at the floor origin. The `XRController` interaction nodes and
`XRHand` visual nodes are scene-root siblings because native XR input already
contains world-space poses. The hands use the engine's default MIT-licensed
skinned models. The floor, table and cube use engine primitives and explicit
box collision shapes in metres. The main scene includes an inspection camera
for desktop editing; OpenXR supplies the headset view during VR play.

## Verify without opening a window

To isolate a lighting artifact on the headset, run the same scene with one
effect disabled at a time from this project directory:

```powershell
.\DiagnoseLighting.ps1 -Mode NoAO
.\DiagnoseLighting.ps1 -Mode NoShadows
.\DiagnoseLighting.ps1 -Mode NoAOOrShadows
```

These are diagnostic comparisons, not renderer fixes. Variants go under the
ignored `build/lighting-diagnostics` directory and leave `scenes/main.scene`
unchanged. Add `-PrepareOnly` to generate the variant without launching anything.

From the engine repository root in PowerShell, with MSYS2 UCRT64 first on `PATH`:

```powershell
cmake -S VRDemo/tools -B VRDemo/build/check -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSaidaEngine_DIR="$PWD/build-rel/sdk"
cmake --build VRDemo/build/check --parallel 2
ctest --test-dir VRDemo/build/check --output-on-failure
```

This loads the project through `Project`, validates the runtime scene schema
and registered node/behaviour types, then exercises the production
`XRGrabbable` with either hand and Jolt gravity/collisions. It starts no window,
renderer, audio device or OpenXR session. Actual optical tracking and reaching
the cube still require a headset test.
