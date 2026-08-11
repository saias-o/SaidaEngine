// Proves the hero car is actually driveable in the city: that it settles on its
// suspension rather than on its chassis, that the throttle moves it along its own
// heading, that steering turns it, that the brake stops it, and that its four
// wheel meshes are placed by the suspension rather than sitting where the scene
// left them. Run with
//   SaidaEngine.exe --project GTAClone --play --test-autoload "E2E=scripts/e2e_drive.js"
//
// It drives through input.inject, the same actions a player's keyboard feeds, so
// what is exercised is the path a player takes and not a private back door.

const REST_Y = 0.40;        // wheel local y at rest: anchor 0.725 - length 0.325
const ANCHOR_X = 0.5313;    // half-track, from vehicles.json via gen_city.py
const ANCHOR_Z = 0.825;     // wheelbase

let frames = 0;
let ok = true;
let restY = null;
let restPos = null;
let driveStart = null;
let headingAtSteer = null;
let suspensionMoved = false;
let peakSpeed = 0.0;

function report(label, pass, detail) {
    console.log("[E2E] " + (pass ? "ok  " : "FAIL") + " " + label +
                (detail === undefined ? "" : "  " + detail));
    if (!pass) ok = false;
    return pass;
}

function car() { return tree.firstInGroup("hero_car"); }

// The car's own forward (+Z) turned into world space by its rotation.
function forwardOf(n) {
    const q = n.getRotation();
    return {
        x: 2.0 * (q.x * q.z + q.w * q.y),
        y: 2.0 * (q.y * q.z - q.w * q.x),
        z: 1.0 - 2.0 * (q.x * q.x + q.y * q.y),
    };
}

function headingDegrees(n) {
    const f = forwardOf(n);
    return Math.atan2(f.x, f.z) * 180.0 / Math.PI;
}

function speedOf(n) {
    const v = n.getVelocity();
    return Math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

function wheels() { return tree.nodesInGroup("vehicle_wheel"); }

function drive(throttle, steer) {
    input.inject("MoveForward", throttle > 0 ? throttle : 0.0);
    input.inject("MoveBackward", throttle < 0 ? -throttle : 0.0);
    input.inject("MoveRight", steer > 0 ? steer : 0.0);
    input.inject("MoveLeft", steer < 0 ? -steer : 0.0);
}

function onUpdate() {
    const c = car();
    if (c === null) {
        if (frames === 0) report("hero car exists", false, "no node in group hero_car");
        frames += 1;
        return;
    }
    frames += 1;

    if (frames > 60) peakSpeed = Math.max(peakSpeed, speedOf(c));

    // ---- settled on its springs ------------------------------------------
    if (frames === 120) {
        const p = c.getPosition();
        restPos = p;
        const w = wheels();
        report("four wheel meshes", w.length === 4, String(w.length));
        // The chassis box is lifted clear of the road, so a car resting on its
        // collider instead of its tyres would sit visibly high or low.
        report("car rests on its tyres", Math.abs(p.y) < 0.12, "y=" + p.y.toFixed(3));
        report("car rests still", speedOf(c) < 0.2, "speed=" + speedOf(c).toFixed(3));

        if (w.length === 4) {
            let placed = 0;
            restY = w[0].getPosition().y;
            for (let i = 0; i < w.length; i += 1) {
                const q = w[i].getPosition();
                // Placed by the suspension, not left where the scene put them.
                if (Math.abs(q.y - REST_Y) < 0.06 &&
                    Math.abs(Math.abs(q.x) - ANCHOR_X) < 0.02 &&
                    Math.abs(Math.abs(q.z) - ANCHOR_Z) < 0.02) placed += 1;
            }
            report("wheels hang where the suspension says", placed === 4,
                   placed + "/4 at y=" + restY.toFixed(3) +
                   " (expected " + REST_Y.toFixed(2) + ")");
        }
        return;
    }

    // ---- get in -----------------------------------------------------------
    // The car only listens to a driver, so the harness becomes one: walk up and
    // press the same key a player would. Disabling the player and injecting past
    // scripts/car.js would prove the physics against a rig no player can reach.
    if (frames === 125) {
        const cp = c.getPosition();
        const player = tree.firstInGroup("player");
        if (player !== null) {
            player.setPosition(cp.x + 1.6, cp.y + 1.4, cp.z);
            player.setVelocity(0.0, 0.0, 0.0);
        }
        input.inject("Interact", 1.0);
        return;
    }
    if (frames === 128) { input.inject("Interact", 0.0); return; }

    // ---- throttle ---------------------------------------------------------
    if (frames === 145) { driveStart = c.getPosition(); drive(1.0, 0.0); return; }

    if (frames > 145 && frames < 355 && restY !== null) {
        const w = wheels();
        // A live suspension keeps changing length under power; a wheel welded to
        // the scene would report the same y for the whole run.
        for (let i = 0; i < w.length; i += 1)
            if (Math.abs(w[i].getPosition().y - restY) > 0.01) suspensionMoved = true;
    }

    if (frames === 355) {
        const p = c.getPosition();
        const f = forwardOf(c);
        const dx = p.x - driveStart.x, dz = p.z - driveStart.z;
        const travelled = Math.sqrt(dx * dx + dz * dz);
        // Projected on the car's own forward, so this is travel the way it
        // points rather than any displacement at all.
        const along = dx * f.x + dz * f.z;

        report("throttle builds speed", peakSpeed > 5.0, peakSpeed.toFixed(2) + " m/s");
        report("car travels along its own forward", along > 15.0 && along > travelled * 0.9,
               "along=" + along.toFixed(2) + " of " + travelled.toFixed(2) + " m");
        report("car stays on the road", Math.abs(p.y - restPos.y) < 0.5,
               "y=" + p.y.toFixed(3));
        report("suspension moves under power", suspensionMoved);

        headingAtSteer = headingDegrees(c);
        drive(1.0, 1.0);
        return;
    }

    // ---- steering ---------------------------------------------------------
    if (frames === 475) {
        let delta = headingDegrees(c) - headingAtSteer;
        while (delta > 180.0) delta -= 360.0;
        while (delta < -180.0) delta += 360.0;
        report("steering turns the car", Math.abs(delta) > 10.0,
               delta.toFixed(1) + " deg");
        drive(0.0, 0.0);
        input.inject("Jump", 1.0);
        return;
    }

    // ---- stopping ---------------------------------------------------------
    if (frames === 715) {
        report("handbrake brings it to rest", speedOf(c) < 1.0,
               speedOf(c).toFixed(3) + " m/s");
        const p = c.getPosition();
        report("nothing went non-finite", isFinite(p.x) && isFinite(p.y) && isFinite(p.z),
               "(" + p.x.toFixed(1) + ", " + p.y.toFixed(1) + ", " + p.z.toFixed(1) + ")");
        console.log(ok ? "[E2E] PASS" : "[E2E] FAIL");
        tree.quit();
    }
}
