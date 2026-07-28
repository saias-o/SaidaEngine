// Measures where a jitter felt while running actually comes from.
//
// Three quantities are sampled every frame while the character runs in a
// straight line at constant speed:
//   * how far the PLAYER moved      — a sawtooth here means the body's transform
//                                     only advances on physics steps, so the
//                                     render frames in between see it stalled;
//   * how far the CAMERA moved      — smooth player + ragged camera means the rig;
//   * the camera's field of view is not readable from a script, so the rig's
//     distance to the pivot stands in for it.
//
// The invariant it gates: a follow rig may lag its target, it may never be
// JERKIER than it. The camera used to measure the target's speed as a one-frame
// finite difference divided by the current frame time, which on an uneven frame
// clock reads as a 3x speed swing and shook look-ahead and the speed-driven fov
// with it — unevenness 1.59 against an input of 0.61.

const SETTLE = 40;
const SAMPLES = 150;

let frames = 0;
let player = null;
let camera = null;

let prevPlayer = null;
let prevCam = null;
let prevDist = null;

let playerSteps = [];
let camSteps = [];
let distReversals = 0;
let lastDistDelta = 0.0;
let stalls = 0;
let dts = [];

function length3(a, b) {
    const dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

function stats(values) {
    if (values.length === 0) return { mean: 0, min: 0, max: 0, jitter: 0 };
    let sum = 0, min = 1e9, max = -1e9;
    for (let i = 0; i < values.length; i += 1) {
        sum += values[i];
        if (values[i] < min) min = values[i];
        if (values[i] > max) max = values[i];
    }
    const mean = sum / values.length;
    // Mean absolute change between consecutive steps, relative to the step size:
    // 0 is perfectly even motion, ~2 is alternating stop/go.
    let churn = 0;
    for (let i = 1; i < values.length; i += 1) churn += Math.abs(values[i] - values[i - 1]);
    return {
        mean: mean, min: min, max: max,
        jitter: mean > 1e-9 ? (churn / (values.length - 1)) / mean : 0,
    };
}

function onReady() {
    player = tree.firstInGroup("player");
    camera = tree.firstInGroup("camera");
}

function onUpdate(dt) {
    frames += 1;
    if (player === null || camera === null) return;

    // Run in a straight line, no turning, no camera input.
    input.inject("MoveForward", true);

    if (frames > SETTLE && frames <= SETTLE + SAMPLES) dts.push(dt);
    if (frames <= SETTLE) return;
    if (frames > SETTLE + SAMPLES) {
        const p = stats(playerSteps);
        const c = stats(camSteps);
        console.log("[JITTER] player step  mean=" + p.mean.toFixed(4) +
                    " min=" + p.min.toFixed(4) + " max=" + p.max.toFixed(4) +
                    " unevenness=" + p.jitter.toFixed(2) +
                    " stalled frames=" + stalls + "/" + playerSteps.length);
        console.log("[JITTER] camera step  mean=" + c.mean.toFixed(4) +
                    " min=" + c.min.toFixed(4) + " max=" + c.max.toFixed(4) +
                    " unevenness=" + c.jitter.toFixed(2));
        const d = stats(dts);
        console.log("[JITTER] frame dt      mean=" + (d.mean * 1000).toFixed(2) +
                    "ms min=" + (d.min * 1000).toFixed(2) +
                    "ms max=" + (d.max * 1000).toFixed(2) +
                    "ms unevenness=" + d.jitter.toFixed(2) +
                    "  (" + (1.0 / Math.max(d.mean, 1e-6)).toFixed(0) + " fps avg)");
        console.log("[JITTER] rig distance reversals=" + distReversals +
                    "/" + camSteps.length);

        // The rig is allowed a little of its own motion (look-ahead, easing);
        // it is not allowed to multiply what it is given.
        const ok = c.jitter <= p.jitter * 1.25;
        console.log("[E2E] " + (ok ? "ok  " : "FAIL") +
                    " camera does not amplify frame-time jitter  camera=" +
                    c.jitter.toFixed(2) + " target=" + p.jitter.toFixed(2));
        console.log(ok ? "[E2E] JITTER PASS" : "[E2E] JITTER FAIL");
        tree.quit();
        return;
    }

    const pp = player.getPosition();
    const cp = camera.getPosition();

    if (prevPlayer !== null) {
        const step = length3(prevPlayer, pp);
        playerSteps.push(step);
        if (step < 1e-5) stalls += 1;
    }
    if (prevCam !== null) camSteps.push(length3(prevCam, cp));

    const dist = length3(pp, cp);
    if (prevDist !== null) {
        const delta = dist - prevDist;
        if (Math.abs(delta) > 0.01) {
            if (lastDistDelta !== 0.0 && (delta > 0.0) !== (lastDistDelta > 0.0)) {
                distReversals += 1;
            }
            lastDistDelta = delta;
        }
    }

    prevPlayer = { x: pp.x, y: pp.y, z: pp.z };
    prevCam = { x: cp.x, y: cp.y, z: cp.z };
    prevDist = dist;
}
