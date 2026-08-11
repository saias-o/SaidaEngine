// Getting into a car, driving it, and getting out again.
//
// Runs on its own node, and neither on a car nor on the player. A character and
// a vehicle read the SAME movement actions, so exactly one thing may be
// listening at a time — and with 32 cars parked around the city, a script per
// car would mean 32 of them racing to answer the same key press. One driver that
// holds at most one car makes that impossible by construction rather than by
// arbitration.
//
// It cannot live on the player either: seating someone disables their node, and
// a disabled node stops running its behaviours. A driver that rode on the player
// would switch itself off the instant it got in, and stay in the car for ever.
//
// Every car is therefore authored with `readsInput` off and is only ever moved
// from here. A traffic AI is the same seam from the other side: it holds its own
// car and calls the same vehicleDrive.

const REACH = 2.2;          // metres of clearance around the player's capsule
const EXIT_SPEED = 2.0;     // m/s below which stepping out is allowed
const EXIT_SIDE = 1.9;      // metres to the side of the car the player lands
const EXIT_LIFT = 1.4;      // the character capsule's own standing height

let car = null;             // the car being driven, or null when on foot
let armed = false;          // debounces the key that got us in or out
let announced = false;

function player() { return tree.firstInGroup("player"); }

function report(text) { console.log("[drive] " + text); }

// The car's own right axis in world space, for putting someone down beside it.
function rightOf(n) {
    const q = n.getRotation();
    return {
        x: 1.0 - 2.0 * (q.y * q.y + q.z * q.z),
        z: 2.0 * (q.x * q.z - q.y * q.w),
    };
}

function distance(a, b) {
    const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

// The nearest vehicle whose COLLIDER the player can reach, not whose origin is
// close: a van is four metres long, so measuring to its centre would let someone
// open it from inside its own bonnet and refuse them at its back doors.
// overlapSphere tests the real shape, so length costs nothing here.
function reachableCar(p) {
    const me = p.getPosition();
    const hits = physics.overlapSphere(me, REACH);
    let best = null;
    let bestDistance = 0.0;
    for (let i = 0; i < hits.length; i += 1) {
        const h = hits[i];
        if (!h.valid() || !h.isInGroup("vehicle")) continue;
        // Several can overlap on a kerb; the closest centre breaks the tie, which
        // is the one the player is standing at.
        const d = distance(me, h.getPosition());
        if (best === null || d < bestDistance) { best = h; bestDistance = d; }
    }
    return best;
}

function enter(p, target) {
    car = target;
    // The camera follows a group rather than a node, so handing the car that
    // membership is what carries the view across. The player keeps its own
    // "player" group so everything else can still find it.
    p.removeFromGroup("camera_target");
    car.addToGroup("camera_target");
    p.setVelocity(0.0, 0.0, 0.0);
    p.setEnabled(false);
    car.vehicleHandbrake(false);
    report("in " + car.getName());
}

function leave(p) {
    if (car === null) return;
    // Left braked rather than coasting: a car nobody is in must not roll away
    // from where its driver stood up.
    car.vehicleDrive(0.0, 0.0);
    car.vehicleBrake(0.0);
    car.vehicleHandbrake(true);

    // Put the player down beside the car rather than inside the shell it
    // collides with, or the character controller shoves them out of it in a
    // direction nobody chose.
    const r = rightOf(car);
    const c = car.getPosition();
    p.setPosition(c.x + r.x * EXIT_SIDE, c.y + EXIT_LIFT, c.z + r.z * EXIT_SIDE);
    p.setVelocity(0.0, 0.0, 0.0);
    p.setEnabled(true);
    car.removeFromGroup("camera_target");
    p.addToGroup("camera_target");
    report("out of " + car.getName());
    car = null;
}

function onReady() {
    input.bindKey("Interact", "F");
    // Say it here as well as in the scene: a car left listening to the shared
    // movement actions drives itself off the kerb the moment the player walks,
    // and a hand-edited scene must not be able to reintroduce that.
    const all = tree.nodesInGroup("vehicle");
    for (let i = 0; i < all.length; i += 1) {
        all[i].vehicleInput(false);
        all[i].vehicleHandbrake(true);
    }
    report("ready, " + all.length + " cars parked");
}

function onUpdate() {
    const p = player();
    if (p === null || !p.valid()) return;
    const pressed = input.justPressed("Interact");

    // The car can be freed under us — a mission could remove it — and every
    // NodeRef call but valid() throws once its target is gone.
    if (car !== null && !car.valid()) {
        car = null;
        p.setEnabled(true);
        p.addToGroup("camera_target");
        report("the car went away");
    }

    if (car === null) {
        if (pressed && !armed) {
            const target = reachableCar(p);
            if (target !== null) { enter(p, target); armed = true; return; }
        }
        if (!pressed) armed = false;
        return;
    }

    // ---- driving -----------------------------------------------------------
    if (pressed && !armed) {
        const state = car.vehicleState();
        if (state !== null && state.speed > EXIT_SPEED) {
            report("too fast to step out: " + state.speed.toFixed(1) + " m/s");
        } else {
            leave(p);
            armed = true;
            return;
        }
    }
    if (!pressed) armed = false;

    // The very actions the character would have read, spent on the car instead.
    const move = input.vector("MoveLeft", "MoveRight", "MoveBackward", "MoveForward");
    car.vehicleDrive(move.y, move.x);
    car.vehicleHandbrake(input.isHeld("Jump"));
}
