// A platform that shuttles along one axis. A StaticBody does not carry a rider,
// so the platform also hands its own frame delta to whoever is standing on it —
// otherwise the player slides off the moment it starts moving.

exportProperty("axis", "x");        // "x" | "y" | "z"
exportProperty("span", 5.0);        // metres either side of the rest position
exportProperty("period", 5.0);      // seconds for a full there-and-back
exportProperty("carryRadius", 3.2); // horizontal reach of the platform's deck
exportProperty("carryHeight", 1.6); // how far above the deck a rider may be

let origin = null;
let player = null;
let phase = 0.0;
let previous = 0.0;

function onReady() {
    const p = node.getPosition();
    origin = { x: p.x, y: p.y, z: p.z };
    player = tree.firstInGroup("player");
    previous = 0.0;
}

function onUpdate(dt) {
    if (origin === null) return;
    const period = props.period > 0.05 ? props.period : 0.05;
    phase += (dt / period) * Math.PI * 2.0;
    const offset = Math.sin(phase) * props.span;
    const delta = offset - previous;
    previous = offset;

    const axis = props.axis;
    node.setPosition(origin.x + (axis === "x" ? offset : 0.0),
                     origin.y + (axis === "y" ? offset : 0.0),
                     origin.z + (axis === "z" ? offset : 0.0));

    if (player === null || Math.abs(delta) < 1e-6) return;
    const here = node.getPosition();
    const p = player.getPosition();
    const dx = p.x - here.x;
    const dz = p.z - here.z;
    const above = p.y - here.y;
    if (dx * dx + dz * dz > props.carryRadius * props.carryRadius) return;
    if (above < 0.0 || above > props.carryHeight) return;

    player.setPosition(p.x + (axis === "x" ? delta : 0.0),
                       p.y + (axis === "y" ? delta : 0.0),
                       p.z + (axis === "z" ? delta : 0.0));
}
