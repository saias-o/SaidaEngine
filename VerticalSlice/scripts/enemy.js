// A ruin guardian. The engine's Character behaviour owns the animation and the
// capsule; this script owns the intent — and because that intent is in world
// space while setMoveInput is camera-relative, the solver is suspended and this
// script integrates the body itself.

exportProperty("hp", 60.0);
exportProperty("speed", 3.2);
exportProperty("damage", 12.0);
exportProperty("aggroRange", 26.0);
exportProperty("attackRange", 1.9);
exportProperty("attackInterval", 1.1);
exportProperty("gravity", 20.0);

let gameState = null;
let player = null;
let death = null;
let health = 0.0;
let dead = false;
let awake = false;
let attackTimer = 0.0;
let verticalSpeed = 0.0;
let home = null;

function distanceTo(target) {
    const a = node.getPosition();
    const b = target.getPosition();
    const dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

function die() {
    if (dead) return;
    dead = true;
    audio.play("enemy_die");
    if (death !== null) {
        death.setEnabled(true);
        time.wait(0.9, function () { death.setEnabled(false); });
    }
    if (gameState !== null) gameState.call("enemyDown");
    node.setVelocity(0.0, 0.0, 0.0);
    // The corpse lingers just long enough for the burst to read, then leaves.
    time.wait(0.9, function () { node.queueFree(); });
}

// Called from the player's shot, across contexts.
function takeDamage(amount) {
    if (dead) return 0;
    health -= Number(amount) || 0;
    awake = true;
    if (health <= 0.0) {
        health = 0.0;
        die();
    }
    return health;
}

function onReady() {
    gameState = tree.autoload("GameState");
    player = tree.firstInGroup("player");
    health = props.hp;
    const p = node.getPosition();
    home = { x: p.x, y: p.y, z: p.z };

    // This script writes the velocity directly, so the built-in solver must not
    // also be writing it — they would fight every frame.
    node.characterSolver(false);

    // A child's getPosition is local, so the burst cannot be found by position;
    // the scene gives each one a group named after its owner instead.
    death = tree.firstInGroup("death_" + node.getName());
    if (death !== null) death.setEnabled(false);
}

function onUpdate(dt) {
    if (dead || player === null) return;
    if (gameState !== null && gameState.call("isOver") === true) {
        node.setVelocity(0.0, 0.0, 0.0);
        return;
    }

    const grounded = node.isOnFloor();
    if (grounded && verticalSpeed < 0.0) verticalSpeed = 0.0;
    verticalSpeed -= props.gravity * dt;

    const range = distanceTo(player);
    if (!awake && range < props.aggroRange) awake = true;

    if (!awake) {
        node.setVelocity(0.0, verticalSpeed, 0.0);
        return;
    }

    const a = node.getPosition();
    const b = player.getPosition();
    let dx = b.x - a.x;
    let dz = b.z - a.z;
    const flat = Math.sqrt(dx * dx + dz * dz);

    if (flat > 1e-3) {
        dx /= flat;
        dz /= flat;
        node.characterFace(dx, dz, false);
    }

    if (flat > props.attackRange) {
        // The Character behaviour still picks idle/run from the body's real
        // velocity while its solver is suspended, so the clips need no driving.
        node.setVelocity(dx * props.speed, verticalSpeed, dz * props.speed);
    } else {
        node.setVelocity(0.0, verticalSpeed, 0.0);
        attackTimer -= dt;
        if (attackTimer <= 0.0) {
            attackTimer = props.attackInterval;
            audio.play("hurt");
            if (gameState !== null) gameState.call("damagePlayer", props.damage);
        }
    }

    // A guardian that walks off the plateau is gone; put it back rather than
    // leaving it falling forever below the level.
    if (a.y < home.y - 30.0) {
        verticalSpeed = 0.0;
        node.setPosition(home.x, home.y, home.z);
    }
}
