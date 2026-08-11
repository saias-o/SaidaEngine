// Getting in and out of a car, and driving it once inside.
//
// Runs on the car itself, which is where a driver belongs — a traffic AI will
// plug into the same seam, reading `node.vehicleDrive` instead of a keyboard.
//
// The one rule this file exists to enforce: a character and a vehicle read the
// SAME movement actions, so exactly one of them may be listening at a time. The
// car is authored with readsInput off and only ever driven from here; the player
// is disabled while seated. Leave both awake and the throttle drives the car and
// walks the player off down the street at once.

const REACH = 3.6;          // metres from the car a player can reach the door
const EXIT_SPEED = 2.0;     // m/s below which stepping out is allowed
const EXIT_SIDE = 1.9;      // metres to the left of the car the player lands
const EXIT_LIFT = 1.4;      // the character capsule's own standing height

let seated = false;
let armed = false;          // debounces the key that got us in

function player() { return tree.firstInGroup("player"); }

// The car's own axes in world space, from its rotation.
function axes() {
    const q = node.getRotation();
    return {
        right: {
            x: 1.0 - 2.0 * (q.y * q.y + q.z * q.z),
            y: 2.0 * (q.x * q.y + q.z * q.w),
            z: 2.0 * (q.x * q.z - q.y * q.w),
        },
        forward: {
            x: 2.0 * (q.x * q.z + q.w * q.y),
            y: 2.0 * (q.y * q.z - q.w * q.x),
            z: 1.0 - 2.0 * (q.x * q.x + q.y * q.y),
        },
    };
}

function distanceTo(p) {
    const a = node.getPosition(), b = p.getPosition();
    const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

function enter(p) {
    seated = true;
    // The camera follows a group rather than a node, so handing the car the
    // membership is what moves the view across. The player keeps its own
    // "player" group so everything else can still find it.
    p.removeFromGroup("camera_target");
    node.addToGroup("camera_target");
    p.setVelocity(0.0, 0.0, 0.0);
    p.setEnabled(false);
    node.vehicleInput(false);   // this script owns the wheel from here
    console.log("[car] entered");
}

function exit(p) {
    seated = false;
    node.vehicleDrive(0.0, 0.0);
    node.vehicleBrake(0.0);
    node.vehicleHandbrake(true);   // left parked, not rolling away

    // Put them down beside the driver's door rather than inside the shell the
    // car collides with, or the character controller will push them out of it in
    // a direction nobody chose.
    const a = axes();
    const c = node.getPosition();
    p.setPosition(c.x + a.right.x * EXIT_SIDE,
                  c.y + EXIT_LIFT,
                  c.z + a.right.z * EXIT_SIDE);
    p.setVelocity(0.0, 0.0, 0.0);
    p.setEnabled(true);
    node.removeFromGroup("camera_target");
    p.addToGroup("camera_target");
    console.log("[car] left");
}

function onReady() {
    input.bindKey("Interact", "F");
    // Authored off, but say so here too: a scene edited by hand must not be able
    // to leave a parked car steering itself whenever the player walks.
    node.vehicleInput(false);
    node.vehicleHandbrake(true);
}

function onUpdate() {
    const p = player();
    if (p === null || !p.valid()) return;

    const pressed = input.justPressed("Interact");

    if (!seated) {
        // Nothing to drive it, so it stays where it was left.
        if (pressed && !armed && distanceTo(p) <= REACH) {
            enter(p);
            armed = true;
            return;
        }
        if (!pressed) armed = false;
        return;
    }

    // ---- seated ------------------------------------------------------------
    const state = node.vehicleState();
    if (pressed && !armed) {
        if (state !== null && state.speed > EXIT_SPEED) {
            console.log("[car] too fast to step out: " + state.speed.toFixed(1) + " m/s");
        } else {
            exit(p);
            armed = true;
            return;
        }
    }
    if (!pressed) armed = false;

    // The same actions the character would have read, now spent on the car.
    const move = input.vector("MoveLeft", "MoveRight", "MoveBackward", "MoveForward");
    node.vehicleDrive(move.y, move.x);
    node.vehicleHandbrake(input.isHeld("Jump"));
}
