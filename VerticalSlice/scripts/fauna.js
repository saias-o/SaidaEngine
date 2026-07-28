// Idle life: a slow breath and a lazy look around. Purely cosmetic — these
// nodes carry no body, so nothing here can affect the simulation.

exportProperty("phase", 0.0);
exportProperty("amplitude", 0.09);
exportProperty("bobSpeed", 1.7);
exportProperty("turnSpeed", 0.25);
exportProperty("turnAmount", 28.0);

let origin = null;
let baseYaw = 0.0;

function onReady() {
    const p = node.getPosition();
    origin = { x: p.x, y: p.y, z: p.z };
    baseYaw = props.phase * 57.29577951;
}

function onUpdate() {
    if (origin === null) return;
    const t = time.elapsed() + props.phase;
    node.setPosition(origin.x,
                     origin.y + Math.abs(Math.sin(t * props.bobSpeed)) * props.amplitude,
                     origin.z);

    const yaw = (baseYaw + Math.sin(t * props.turnSpeed) * props.turnAmount) * 0.017453292;
    node.setRotation(0.0, Math.sin(yaw * 0.5), 0.0, Math.cos(yaw * 0.5));
}
