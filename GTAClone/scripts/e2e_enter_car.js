// Proves the round trip a player actually makes: walk up to the car, get in,
// drive it away, get out, and walk again — with the camera following whichever
// of the two is in charge, and never both moving at once. Run with
//   SaidaEngine.exe --project GTAClone --play --test-autoload "E2E=scripts/e2e_enter_car.js"
//
// The thing under test is the handover. scripts/e2e_drive.js already covers the
// driving itself, and it disables the player to do so; this one leaves the
// player alive throughout, which is the only way the conflict shows up.

let frames = 0;
let ok = true;
let onFootStart = null;
let seatedCarStart = null;
let playerWhileSeated = null;
let carAfterExit = null;
let phase = "onfoot";
let phaseFrame = 0;

function report(label, pass, detail) {
    console.log("[E2E] " + (pass ? "ok  " : "FAIL") + " " + label +
                (detail === undefined ? "" : "  " + detail));
    if (!pass) ok = false;
    return pass;
}

function car() { return tree.firstInGroup("hero_car"); }
function player() { return tree.firstInGroup("player"); }

function dist(a, b) {
    const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

// Whoever the camera is following this frame.
function cameraTargetName() {
    const t = tree.firstInGroup("camera_target");
    return t === null ? "(none)" : t.getName();
}

function hold(action, on) { input.inject(action, on ? 1.0 : 0.0); }

function speedOf(n) {
    const v = n.getVelocity();
    return Math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

function onUpdate() {
    const c = car(), p = player();
    if (c === null || p === null) {
        if (frames === 0) report("car and player exist", false);
        frames += 1;
        return;
    }
    frames += 1;

    // ---- on foot: the car must ignore the movement keys ------------------
    if (frames === 90) {
        onFootStart = c.getPosition();
        report("camera starts on the player", cameraTargetName() === "Player",
               cameraTargetName());
        hold("MoveForward", true);
        return;
    }
    if (frames === 180) {
        // The player has been walking for 1.5 s. A car still listening to the
        // same actions would have driven off with them.
        const moved = dist(c.getPosition(), onFootStart);
        report("a parked car ignores the walk keys", moved < 0.35,
               "car moved " + moved.toFixed(3) + " m");
        hold("MoveForward", false);
        // Stand next to the driver's door and reach for it.
        const cp = c.getPosition();
        p.setPosition(cp.x + 1.6, cp.y + 1.4, cp.z);
        p.setVelocity(0.0, 0.0, 0.0);
        return;
    }

    // ---- get in ------------------------------------------------------------
    if (frames === 210) { hold("Interact", true); return; }
    if (frames === 213) { hold("Interact", false); return; }

    if (frames === 240) {
        report("camera moves to the car", cameraTargetName() === "HeroCar",
               cameraTargetName());
        playerWhileSeated = p.getPosition();
        seatedCarStart = c.getPosition();
        hold("MoveForward", true);
        return;
    }

    // ---- drive -------------------------------------------------------------
    if (frames === 420) {
        const travelled = dist(c.getPosition(), seatedCarStart);
        report("the car drives once someone is in it", travelled > 8.0,
               travelled.toFixed(2) + " m");
        // And the player went with it rather than staying on the pavement or
        // walking off under the same keys.
        const drift = dist(p.getPosition(), playerWhileSeated);
        report("the player does not walk while seated", drift < 0.5,
               "drifted " + drift.toFixed(3) + " m");
        hold("MoveForward", false);
        hold("Jump", true);          // the handbrake, as car.js maps it
        phase = "slowing";
        return;
    }

    // Stepping out of a moving car is refused by design, so wait for it to stop
    // rather than counting frames — coasting down from 25 m/s takes as long as
    // it takes, and a harness that guessed would be testing its own arithmetic.
    if (phase === "slowing") {
        if (speedOf(c) < 1.0) {
            hold("Jump", false);
            hold("Interact", true);
            phase = "leaving";
            phaseFrame = frames;
        } else if (frames > 1200) {
            report("the car slows enough to step out", false,
                   speedOf(c).toFixed(2) + " m/s");
            console.log("[E2E] FAIL");
            tree.quit();
        }
        return;
    }

    if (phase === "leaving") {
        if (frames === phaseFrame + 3) { hold("Interact", false); return; }
        if (frames !== phaseFrame + 40) return;

        report("camera returns to the player", cameraTargetName() === "Player",
               cameraTargetName());
        const gap = dist(p.getPosition(), c.getPosition());
        report("the player is put down beside the car", gap > 1.0 && gap < 4.0,
               gap.toFixed(2) + " m away");
        carAfterExit = c.getPosition();
        hold("MoveForward", true);   // walk again, under the very same action
        phase = "walking";
        phaseFrame = frames;
        return;
    }

    // ---- on foot again -----------------------------------------------------
    if (phase === "walking" && frames === phaseFrame + 120) {
        const carMoved = dist(c.getPosition(), carAfterExit);
        report("the car stays parked once left", carMoved < 0.6,
               "car moved " + carMoved.toFixed(3) + " m");
        report("the player walks again after getting out",
               dist(p.getPosition(), carAfterExit) > 2.0);
        hold("MoveForward", false);
        const p2 = p.getPosition();
        report("nothing went non-finite",
               isFinite(p2.x) && isFinite(p2.y) && isFinite(p2.z));
        console.log(ok ? "[E2E] PASS" : "[E2E] FAIL");
        tree.quit();
    }
}
