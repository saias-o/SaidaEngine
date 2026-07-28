// Player script. Movement itself is the engine's Character behaviour — this adds
// what belongs to this game: aiming, the shot with its bolt and impact, footsteps,
// the fall-out-of-the-world rule and the restart key.
//
// The shot is hitscan (one raycast decides the hit immediately) with a bolt that
// then flies to the point it already hit: accurate to aim, readable on screen.
// Scripts cannot create nodes, so every bolt and impact comes from a pool the
// scene declares.

exportProperty("fireRate", 0.14);       // seconds between shots
exportProperty("damage", 34.0);
exportProperty("range", 80.0);
exportProperty("boltSpeed", 85.0);      // m/s the visible bolt travels
exportProperty("muzzleHeight", 0.5);    // above the body origin, i.e. chest height:
                                        // the origin is the capsule centre, not the feet
exportProperty("muzzleForward", 0.8);   // ahead of the capsule, so the ray clears it
exportProperty("killPlaneY", -25.0);
exportProperty("respawnX", 0.0);
exportProperty("respawnY", 1.4);
exportProperty("respawnZ", 14.0);

let gameState = null;
let camera = null;
let hand = null;
let muzzle = null;
let impacts = [];
let bolts = [];
let live = [];              // bolts currently in flight
let impactCursor = 0;
let boltCursor = 0;

let fireCooldown = 0.0;
let stepTimer = 0.0;
let stepFlip = false;
let wasAirborne = false;
let handRest = null;
let recoil = 0.0;

function rotate(q, vx, vy, vz) {
    const x = q.x, y = q.y, z = q.z, w = q.w;
    const tx = 2.0 * (y * vz - z * vy);
    const ty = 2.0 * (z * vx - x * vz);
    const tz = 2.0 * (x * vy - y * vx);
    return {
        x: vx + w * tx + (y * tz - z * ty),
        y: vy + w * ty + (z * tx - x * tz),
        z: vz + w * tz + (x * ty - y * tx),
    };
}

// Rotation taking local -Z onto `dir` — how the engine orients a node, and so
// how the elongated bolt mesh ends up pointing along the shot.
function lookRotation(dx, dy, dz) {
    const len = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-5) return { x: 0, y: 0, z: 0, w: 1 };
    const fx = dx / len, fy = dy / len, fz = dz / len;
    const yaw = Math.atan2(-fx, -fz);
    const pitch = Math.asin(Math.max(-1.0, Math.min(1.0, fy)));
    const cy = Math.cos(yaw * 0.5), sy = Math.sin(yaw * 0.5);
    const cp = Math.cos(pitch * 0.5), sp = Math.sin(pitch * 0.5);
    return { x: cy * sp, y: sy * cp, z: -sy * sp, w: cy * cp };
}

function aimDirection() {
    if (camera === null) return { x: 0, y: 0, z: -1 };
    const q = camera.getRotation();
    if (q === null) return { x: 0, y: 0, z: -1 };
    return rotate(q, 0, 0, -1);
}

function popImpact(x, y, z) {
    if (impacts.length === 0) return;
    const fx = impacts[impactCursor % impacts.length];
    impactCursor += 1;
    fx.setPosition(x, y, z);
    fx.setEnabled(true);
    time.wait(0.25, function () {
        fx.setEnabled(false);
        fx.setPosition(0.0, -200.0, 0.0);
    });
}

function launchBolt(ox, oy, oz, dir, distance, onArrival) {
    if (bolts.length === 0) {
        onArrival();
        return;
    }
    const b = bolts[boltCursor % bolts.length];
    boltCursor += 1;
    const r = lookRotation(dir.x, dir.y, dir.z);
    b.setRotation(r.x, r.y, r.z, r.w);
    b.setPosition(ox, oy, oz);
    b.setEnabled(true);
    live.push({
        node: b, x: ox, y: oy, z: oz, dir: dir,
        travelled: 0.0, distance: distance, arrive: onArrival,
    });
}

function stepBolts(dt) {
    if (live.length === 0) return;
    const step = props.boltSpeed * dt;
    const keep = [];
    for (let i = 0; i < live.length; i += 1) {
        const b = live[i];
        b.travelled += step;
        if (b.travelled >= b.distance) {
            b.node.setEnabled(false);
            b.node.setPosition(0.0, -200.0, 0.0);
            b.arrive();
            continue;
        }
        b.node.setPosition(b.x + b.dir.x * b.travelled,
                           b.y + b.dir.y * b.travelled,
                           b.z + b.dir.z * b.travelled);
        keep.push(b);
    }
    live = keep;
}

// What the crosshair is over. The camera sits behind and over the shoulder, so
// its forward axis and the muzzle's are not the same line: firing straight down
// the camera axis lands the shot beside whatever the player is pointing at. The
// crosshair therefore picks the point, and the muzzle then aims AT that point.
//
// The camera ray has to start past the player: `ignoreSelf` does not cover a
// CharacterBody's inner body (SPEC 5.1), so a ray from the camera would report
// the player's own capsule at nearly zero distance.
function crosshairPoint(dir) {
    const cam = camera === null ? null : camera.getPosition();
    if (cam === null) return null;

    const p = node.getPosition();
    const ahead = (p.x - cam.x) * dir.x + (p.y - cam.y) * dir.y + (p.z - cam.z) * dir.z;
    const clearance = Math.max(0.0, ahead) + 0.8;
    const origin = {
        x: cam.x + dir.x * clearance,
        y: cam.y + dir.y * clearance,
        z: cam.z + dir.z * clearance,
    };
    const seen = physics.raycast(origin, dir, props.range, { ignoreSelf: true });
    if (seen !== null && seen.distance > 0.15) {
        return { x: seen.point.x, y: seen.point.y, z: seen.point.z };
    }
    const far = clearance + props.range;
    return { x: cam.x + dir.x * far, y: cam.y + dir.y * far, z: cam.z + dir.z * far };
}

function fire() {
    const p = node.getPosition();
    const view = aimDirection();
    const aimAt = crosshairPoint(view);

    // The barrel starts at chest height, then steps forward along the shot so the
    // ray begins outside the player's own capsule.
    let dir = view;
    if (aimAt !== null) {
        const dx = aimAt.x - p.x;
        const dy = aimAt.y - (p.y + props.muzzleHeight);
        const dz = aimAt.z - p.z;
        const len = Math.sqrt(dx * dx + dy * dy + dz * dz);
        if (len > 1e-3) dir = { x: dx / len, y: dy / len, z: dz / len };
    }

    const ox = p.x + dir.x * props.muzzleForward;
    const oy = p.y + props.muzzleHeight + dir.y * props.muzzleForward;
    const oz = p.z + dir.z * props.muzzleForward;

    stepFlip = !stepFlip;
    audio.play(stepFlip ? "shoot" : "shoot_alt");
    recoil = 1.0;
    if (muzzle !== null) {
        muzzle.setEnabled(true);
        time.wait(0.06, function () { muzzle.setEnabled(false); });
    }

    const hit = physics.raycast({ x: ox, y: oy, z: oz }, dir, props.range,
                                { ignoreSelf: true });

    if (hit === null || hit.distance < 0.15) {
        launchBolt(ox, oy, oz, dir, props.range, function () {});
        return;
    }

    // The hit is already decided; the bolt only has to arrive and report it.
    // What it hit can be gone by the time it lands — an earlier bolt may have
    // finished it — and every other NodeRef method throws on a dead target, so
    // the reference is tested with valid() first.
    const point = { x: hit.point.x, y: hit.point.y, z: hit.point.z };
    const struck = hit.node;
    launchBolt(ox, oy, oz, dir, hit.distance, function () {
        popImpact(point.x, point.y, point.z);
        if (struck === null || !struck.valid()) return;
        if (struck.isInGroup("enemy")) {
            audio.play("hit");
            struck.call("takeDamage", props.damage);
        } else if (struck.isInGroup("target")) {
            struck.call("takeDamage", props.damage);
        }
    });
}

function respawn() {
    node.setVelocity(0.0, 0.0, 0.0);
    node.setPosition(props.respawnX, props.respawnY, props.respawnZ);
    if (gameState !== null) {
        gameState.call("damagePlayer", 20.0);
        gameState.call("announce", "AIE — retour au depart", 2.0);
    }
    audio.play("hurt");
}

function onReady() {
    gameState = tree.autoload("GameState");
    if (gameState === null) throw new Error("GameState autoload is missing");

    // Fire already sits on the left mouse button in the engine defaults, so the
    // profile is added to rather than replaced — replacing it would drop every
    // movement binding this game still relies on.
    input.bindKey("Restart", "R");

    camera = tree.firstInGroup("camera");
    hand = tree.firstInGroup("hand");
    muzzle = tree.firstInGroup("muzzle");
    if (hand !== null) {
        const r = hand.getPosition();
        handRest = [r.x, r.y, r.z];
    }

    impacts = tree.nodesInGroup("impact");
    bolts = tree.nodesInGroup("tracer");
    for (let i = 0; i < bolts.length; i += 1) bolts[i].setEnabled(false);
    for (let i = 0; i < impacts.length; i += 1) impacts[i].setEnabled(false);
    if (muzzle !== null) muzzle.setEnabled(false);

    console.log("[Verdance] player ready — bolts=" + bolts.length +
                " impacts=" + impacts.length + " camera=" + (camera !== null));
}

function onUpdate(dt) {
    if (input.justPressed("Restart")) {
        tree.reloadScene();
        return;
    }

    const over = gameState !== null && gameState.call("isOver") === true;

    if (!over) {
        fireCooldown -= dt;
        if (input.isHeld("Fire") && fireCooldown <= 0.0) {
            fireCooldown = props.fireRate;
            fire();
        }
    }
    stepBolts(dt);

    // Footsteps and landing, read from the controller's own published state.
    const state = node.characterState();
    if (state !== null) {
        if (state.grounded && state.speed > 1.2) {
            stepTimer -= dt * (state.speed / 5.0);
            if (stepTimer <= 0.0) {
                stepTimer = 0.36;
                audio.play(stepTimer > 0 && stepFlip ? "step_a" : "step_b");
            }
        }
        if (!state.grounded) {
            wasAirborne = true;
        } else if (wasAirborne) {
            wasAirborne = false;
            audio.play("land");
        }
    }

    // The weapon kicks and settles. The rig's bones are not addressable from a
    // script, so this node carries the whole recoil.
    if (hand !== null && handRest !== null) {
        recoil = Math.max(0.0, recoil - dt * 9.0);
        const kick = recoil * recoil * 0.14;
        hand.setPosition(handRest[0], handRest[1] - kick * 0.35, handRest[2] + kick);
    }

    const p = node.getPosition();
    if (p.y < props.killPlaneY) respawn();
}
