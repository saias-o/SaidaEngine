// Regression proof for the follow camera's de-occlusion.
//
// Pitching the rig down drives it into the ground, which is the case where the
// old code oscillated: it probed the segment the camera CURRENTLY occupied and
// wrote the correction back into the smoothing state, so a pulled-in camera
// cast a ray too short to see the wall that pulled it in, eased out, saw it
// again, and flipped between two distances every frame.
//
// The measure is the frame-to-frame change in camera distance: a smooth rig
// changes it in one direction at a time, an oscillating one reverses on almost
// every frame.
//
//   SaidaEngine.exe --project VerticalSlice --play \
//       --test-autoload "E2E=scripts/e2e_camera.js"

const SETTLE = 60;          // frames spent pitching before anything is measured
const SAMPLES = 120;        // frames measured
const FLIP = 0.10;          // metres: a reversal smaller than this is noise
const MAX_REVERSAL_RATIO = 0.25;
const MAX_SWING = 1.5;      // metres between the closest and furthest sample

let frames = 0;
let player = null;
let camera = null;
let gameplayRequested = false;

let previous = null;
let lastDelta = 0.0;
let reversals = 0;
let samples = 0;
let low = 1e9;
let high = -1e9;

function distance() {
    const a = player.getPosition();
    const b = camera.getPosition();
    const dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

function onReady() {
    player = tree.firstInGroup("player");
    camera = tree.firstInGroup("camera");
}

function onUpdate() {
    if (player === null || camera === null) {
        player = tree.firstInGroup("player");
        camera = tree.firstInGroup("camera");
        if (player === null && !gameplayRequested) {
            gameplayRequested = true;
            tree.changeScene("scenes/verdance.scene");
        }
        return;
    }
    frames += 1;

    // Hold the rig hard against its downward pitch limit for the whole run, so
    // it stays in the configuration that used to fight itself.
    input.inject("LookUp", true);

    if (frames <= SETTLE) return;
    if (frames > SETTLE + SAMPLES) {
        const ratio = samples > 0 ? reversals / samples : 0.0;
        const swing = high - low;

        const steady = ratio <= MAX_REVERSAL_RATIO;
        const bounded = swing <= MAX_SWING;
        console.log("[E2E] " + (steady ? "ok  " : "FAIL") +
                    " camera does not oscillate  reversals=" +
                    reversals + "/" + samples + " (" + ratio.toFixed(2) + ")");
        console.log("[E2E] " + (bounded ? "ok  " : "FAIL") +
                    " camera distance stays bounded  " + low.toFixed(2) +
                    ".." + high.toFixed(2) + " m");
        console.log(steady && bounded ? "[E2E] CAMERA PASS" : "[E2E] CAMERA FAIL");
        tree.quit();
        return;
    }

    const d = distance();
    if (d < low) low = d;
    if (d > high) high = d;

    if (previous !== null) {
        const delta = d - previous;
        if (Math.abs(delta) > FLIP) {
            if (lastDelta !== 0.0 && (delta > 0.0) !== (lastDelta > 0.0)) reversals += 1;
            lastDelta = delta;
        }
        samples += 1;
    }
    previous = d;
}
