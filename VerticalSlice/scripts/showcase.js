// Drives the player for a capture: walks forward, looks around, shoots the
// tutorial targets, then holds still. Only used to record the slice.
let t = 0;

function onUpdate(dt) {
    t += dt;
    if (t < 1.0) return;

    if (t < 4.5) {
        input.inject("MoveForward", true);
        input.inject("Fire", t % 0.7 < 0.25);
    } else if (t < 5.2) {
        input.inject("MoveForward", false);
        input.inject("Fire", false);
        input.inject("Jump", true);
    } else {
        input.inject("Jump", false);
        input.inject("Fire", t % 1.2 < 0.3);
    }
    if (t > 30.0) tree.quit();
}
