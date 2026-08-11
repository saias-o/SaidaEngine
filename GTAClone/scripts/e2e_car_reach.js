// Which car opens, and from where. Two claims that are easy to state and easy to
// get wrong, so they are measured here rather than asserted in a comment:
//
//   1. Reach is against the car's COLLIDER, not its origin. A van is four metres
//      long; measuring to its centre would open it from inside its own bonnet
//      and refuse the same player standing at its back doors.
//   2. With 32 cars parked around the city, the one you are standing at is the
//      one that opens — not a fixed favourite, and not whichever the engine
//      happened to list first.
//
// Run with
//   SaidaEngine.exe --project GTAClone --play --test-autoload "E2E=scripts/e2e_car_reach.js"

const BEYOND_ORIGIN = 2.6;  // metres: past REACH (2.2) measured to the origin,
                            // still against the bumper of any car in the kit

let frames = 0;
let ok = true;
let step = 0;
let stepFrame = 0;
let targets = [];

function report(label, pass, detail) {
    console.log("[E2E] " + (pass ? "ok  " : "FAIL") + " " + label +
                (detail === undefined ? "" : "  " + detail));
    if (!pass) ok = false;
    return pass;
}

function player() { return tree.firstInGroup("player"); }
function seatedIn() {
    const t = tree.firstInGroup("camera_target");
    return t === null ? "(none)" : t.getName();
}

function forwardOf(n) {
    const q = n.getRotation();
    return { x: 2.0 * (q.x * q.z + q.w * q.y), z: 1.0 - 2.0 * (q.x * q.x + q.y * q.y) };
}

function standAt(p, car, alongForward, sideways) {
    const c = car.getPosition();
    const f = forwardOf(car);
    // Sideways is forward turned a quarter turn in the XZ plane.
    p.setPosition(c.x + f.x * alongForward - f.z * sideways,
                  c.y + 1.4,
                  c.z + f.z * alongForward + f.x * sideways);
    p.setVelocity(0.0, 0.0, 0.0);
}

function tapInteract(on) { input.inject("Interact", on ? 1.0 : 0.0); }

// Each step is: stand somewhere, tap the key, look at who we are sitting in.
// The gaps let the character settle and the handover run.
function advance() { step += 1; stepFrame = frames; }

function onUpdate() {
    frames += 1;
    const p = player();
    if (p === null) return;

    if (frames === 60) {
        targets = tree.nodesInGroup("vehicle");
        report("the city is full of cars", targets.length > 8, targets.length + " of them");
        advance();
        return;
    }
    if (step === 0) return;
    const t = frames - stepFrame;

    // ---- 1. reach is against the shape, not the origin --------------------
    if (step === 1) {
        if (t === 1) { standAt(p, tree.firstInGroup("hero_car"), BEYOND_ORIGIN, 0.0); return; }
        if (t === 30) { tapInteract(true); return; }
        if (t === 33) { tapInteract(false); return; }
        if (t === 60) {
            report("a car opens from beyond its own centre", seatedIn() === "HeroCar",
                   BEYOND_ORIGIN.toFixed(1) + " m from the origin, got " + seatedIn());
            tapInteract(true);
            return;
        }
        if (t === 63) { tapInteract(false); return; }
        if (t === 90) {
            report("and closes again", seatedIn() === "Player", seatedIn());
            advance();
        }
        return;
    }

    // ---- 2. the car you are standing at is the one that opens -------------
    // Two different cars in turn, so passing cannot mean "always the hero".
    if (step === 2 || step === 3) {
        const pick = targets[step === 2 ? 0 : Math.min(7, targets.length - 1)];
        if (t === 1) { standAt(p, pick, 0.0, 1.7); return; }
        if (t === 30) { tapInteract(true); return; }
        if (t === 33) { tapInteract(false); return; }
        if (t === 60) {
            report("standing at " + pick.getName() + " opens that car",
                   seatedIn() === pick.getName(), "got " + seatedIn());
            tapInteract(true);
            return;
        }
        if (t === 63) { tapInteract(false); return; }
        if (t === 90) {
            report("out of " + pick.getName(), seatedIn() === "Player", seatedIn());
            advance();
        }
        return;
    }

    // ---- 3. standing nowhere near a car opens nothing ----------------------
    if (step === 4) {
        if (t === 1) {
            const c = targets[0].getPosition();
            p.setPosition(c.x, c.y + 1.4, c.z + 40.0);   // well clear of the row
            p.setVelocity(0.0, 0.0, 0.0);
            return;
        }
        if (t === 30) { tapInteract(true); return; }
        if (t === 33) { tapInteract(false); return; }
        if (t === 60) {
            report("pressing the key in the open street does nothing",
                   seatedIn() === "Player", seatedIn());
            console.log(ok ? "[E2E] PASS" : "[E2E] FAIL");
            tree.quit();
        }
        return;
    }
}
