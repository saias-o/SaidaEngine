// Proves the hero car is actually driveable in the city: that it settles on its
// suspension rather than on its chassis, that its four wheel meshes are placed
// by the suspension, that the throttle moves it along its own heading, that
// steering turns it the way it is asked to, and that the handbrake stops it.
//
//   SaidaEngine.exe --project GTAClone --play --test-autoload "E2E=scripts/e2e_drive.js"
//   http://localhost:PORT/?smoke&report&test-autoload=E2E%3Dscripts%2Fe2e_drive.js
//
// Keyed on elapsed time, never on a frame count. The desktop build runs this
// city at a few hundred frames a second and the Web build at a handful, so a
// harness that counted frames measured a settled car on one backend and a
// mid-bounce one on the other — and reported it as a suspension defect.
//
// It gets in the way a player does, through input.inject and scripts/driver.js,
// rather than disabling the player and driving the car directly: proving the
// physics against a rig nobody can reach would prove the wrong thing.

const REST_Y = 0.40;        // wheel local y at rest: anchor 0.725 - length 0.325
const ANCHOR_X = 0.5313;    // half-track, from vehicles.json via gen_city.py
const ANCHOR_Z = 0.825;     // wheelbase

let phase = 0;
let ok = true;
// Collected so the last line carries the whole verdict. On Web the console ring
// is flooded by renderer warnings long before a reader gets to it, and the dev
// server's /__saida_e2e endpoint keeps only the most recent line — so a run that
// reports its failures one at a time reports them where nobody can read them.
let failures = [];
let restY = null;
let restPos = null;
let driveStart = null;
let turnTotal = 0.0;
let lastHeading = null;
let suspensionMoved = false;
let peakSpeed = 0.0;

function report(label, pass, detail) {
    console.log("[E2E] " + (pass ? "ok  " : "FAIL") + " " + label +
                (detail === undefined ? "" : "  " + detail));
    if (!pass) { ok = false; failures.push(label); }
    return pass;
}

function car() { return tree.firstInGroup("hero_car"); }
// The hero car's own four, not the fleet's hundred and twenty: gen_city.py
// groups every car's wheels under its own name as well as under vehicle_wheel,
// and asking for the shared group once the whole fleet became driveable counted
// every parked car's wheels as though they were this one's.
function wheels() { return tree.nodesInGroup("HeroCar_wheel"); }

// The car's own forward (+Z) turned into world space by its rotation.
function forwardOf(n) {
    const q = n.getRotation();
    return {
        x: 2.0 * (q.x * q.z + q.w * q.y),
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

function drive(throttle, steer) {
    input.inject("MoveForward", throttle > 0 ? throttle : 0.0);
    input.inject("MoveBackward", throttle < 0 ? -throttle : 0.0);
    input.inject("MoveRight", steer > 0 ? steer : 0.0);
    input.inject("MoveLeft", steer < 0 ? -steer : 0.0);
}

function onUpdate() {
    const c = car();
    if (c === null) return;
    const t = time.elapsed();

    if (t > 1.0) peakSpeed = Math.max(peakSpeed, speedOf(c));

    // Suspension length keeps changing under power; a wheel left where the scene
    // put it would report the same y for the whole run.
    if (phase >= 3 && phase <= 4 && restY !== null) {
        const w = wheels();
        for (let i = 0; i < w.length; i += 1)
            if (Math.abs(w[i].getPosition().y - restY) > 0.01) suspensionMoved = true;
    }

    if (phase >= 4 && phase <= 5 && lastHeading !== null) {
        let d = headingDegrees(c) - lastHeading;
        while (d > 180.0) d -= 360.0;
        while (d < -180.0) d += 360.0;
        turnTotal += d;
        lastHeading = headingDegrees(c);
    }

    // ---- settled on its springs ------------------------------------------
    if (phase === 0 && t >= 3.0) {
        const p = c.getPosition();
        restPos = p;
        const w = wheels();
        report("four wheel meshes", w.length === 4, String(w.length));
        // The chassis box is lifted clear of the road, so a car resting on its
        // collider instead of its tyres would sit visibly high or low.
        report("car rests on its tyres", Math.abs(p.y) < 0.12, "y=" + p.y.toFixed(3));
        report("car rests still", speedOf(c) < 0.3, "speed=" + speedOf(c).toFixed(3));

        if (w.length === 4) {
            let placed = 0;
            restY = w[0].getPosition().y;
            for (let i = 0; i < w.length; i += 1) {
                const q = w[i].getPosition();
                if (Math.abs(q.y - REST_Y) < 0.06 &&
                    Math.abs(Math.abs(q.x) - ANCHOR_X) < 0.02 &&
                    Math.abs(Math.abs(q.z) - ANCHOR_Z) < 0.02) placed += 1;
            }
            report("wheels hang where the suspension says", placed === 4,
                   placed + "/4 at y=" + restY.toFixed(3) +
                   " (expected " + REST_Y.toFixed(2) + ")");
        }
        phase = 1;
        return;
    }

    // ---- get in -----------------------------------------------------------
    if (phase === 1 && t >= 3.2) {
        const cp = c.getPosition();
        const player = tree.firstInGroup("player");
        if (player !== null) {
            player.setPosition(cp.x + 1.6, cp.y + 1.4, cp.z);
            player.setVelocity(0.0, 0.0, 0.0);
        }
        input.inject("Interact", 1.0);
        phase = 2;
        return;
    }
    if (phase === 2 && t >= 3.35) { input.inject("Interact", 0.0); phase = 3; return; }

    // ---- throttle ---------------------------------------------------------
    if (phase === 3 && t >= 3.6 && driveStart === null) {
        driveStart = c.getPosition();
        drive(1.0, 0.0);
        return;
    }

    if (phase === 3 && t >= 7.2) {
        const p = c.getPosition();
        const f = forwardOf(c);
        const dx = p.x - driveStart.x, dz = p.z - driveStart.z;
        const travelled = Math.sqrt(dx * dx + dz * dz);
        // Projected on the car's own forward, so this is travel the way it
        // points rather than any displacement at all.
        const along = dx * f.x + dz * f.z;

        report("throttle builds speed", peakSpeed > 5.0, peakSpeed.toFixed(2) + " m/s");
        report("car travels along its own forward", along > 12.0 && along > travelled * 0.9,
               "along=" + along.toFixed(2) + " of " + travelled.toFixed(2) + " m");
        report("car stays on the road", Math.abs(p.y - restPos.y) < 0.5,
               "y=" + p.y.toFixed(3));
        report("suspension moves under power", suspensionMoved);

        lastHeading = headingDegrees(c);
        drive(1.0, 1.0);
        phase = 4;
        return;
    }

    // ---- steering ---------------------------------------------------------
    if (phase === 4 && t >= 11.2) {
        // Negative because the car's +X is its left, so steering right takes the
        // nose toward -X and the heading falls. Direction, not just movement: a
        // check that only asks whether it turned passes on mirrored controls.
        report("steering turns the car to the right", turnTotal < -10.0,
               turnTotal.toFixed(1) + " deg");
        drive(0.0, 0.0);
        input.inject("Jump", 1.0);   // the handbrake, as driver.js maps it
        phase = 5;
        return;
    }

    // ---- stopping ---------------------------------------------------------
    if (phase === 5 && t >= 21.0) {
        report("handbrake brings it to rest", speedOf(c) < 1.0,
               speedOf(c).toFixed(3) + " m/s");
        const p = c.getPosition();
        report("nothing went non-finite", isFinite(p.x) && isFinite(p.y) && isFinite(p.z),
               "(" + p.x.toFixed(1) + ", " + p.y.toFixed(1) + ", " + p.z.toFixed(1) + ")");
        console.log(ok ? "[E2E] PASS" : "[E2E] FAIL: " + failures.join("; "));
        phase = 6;
        tree.quit();
    }
}
