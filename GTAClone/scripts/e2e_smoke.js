// Structural and functional check for the city scene: proves the kits load, the
// layout is the one the generator declares, the player stands on the ground, and
// a street door actually moves them into its interior. Run with
//   SaidaEngine.exe --project GTAClone --play --test-autoload "E2E=scripts/e2e_smoke.js"

const INTERIOR_Y = -60.0;   // must match gen_city.py

let frames = 0;
let ok = true;
let startY = null;
let doorPos = null;

function report(label, pass, detail) {
    console.log("[E2E] " + (pass ? "ok  " : "FAIL") + " " + label +
                (detail === undefined ? "" : "  " + detail));
    if (!pass) ok = false;
    return pass;
}

function structure() {
    const player = tree.firstInGroup("player");
    report("camera", tree.firstInGroup("camera") !== null);
    report("physics", physics.available());

    const p = player.getPosition();
    // The ground collider's top is y = 0 and the capsule is 1.75 m tall, so a
    // grounded player sits near 0.875. Falling through shows up as a large drop.
    report("player grounded", p.y > 0.5 && p.y < 2.0,
           "y=" + p.y.toFixed(2) + " from " + startY.toFixed(2));

    // The layout the generator declares. A model that failed to load leaves its
    // node in place, so these are structural only: what proves the meshes
    // actually read is the harness grep for [error]/[warn] on the run.
    const counts = {
        road: tree.nodesInGroup("road").length,
        building: tree.nodesInGroup("building").length,
        car: tree.nodesInGroup("parked_car").length,
        lamp: tree.nodesInGroup("streetlight").length,
        skyline: tree.nodesInGroup("skyline").length,
        interior: tree.nodesInGroup("interior").length,
        streetDoor: tree.nodesInGroup("street_door").length,
        exitDoor: tree.nodesInGroup("interior_exit").length,
    };
    report("road tiles", counts.road === 568, String(counts.road));
    report("buildings", counts.building === 390, String(counts.building));
    report("parked cars", counts.car === 32, String(counts.car));
    report("street lights", counts.lamp === 98, String(counts.lamp));
    report("skyline", counts.skyline === 86, String(counts.skyline));
    report("interiors", counts.interior === 4, String(counts.interior));
    report("doors paired", counts.streetDoor === 4 && counts.exitDoor === 4,
           counts.streetDoor + "/" + counts.exitDoor);
    report("sea", tree.firstInGroup("sea") !== null);
    report("sand", tree.firstInGroup("sand") !== null);
    report("dock", tree.firstInGroup("dock") !== null);
    report("boat", tree.firstInGroup("boat") !== null);
}

function onUpdate() {
    const player = tree.firstInGroup("player");
    if (player === null) return;
    frames += 1;
    if (frames === 1) startY = player.getPosition().y;

    // Long enough for the character to have settled onto the collider.
    if (frames === 150) {
        structure();
        return;
    }

    // Step into a street door and check it puts the player in an interior. The
    // interiors sit far below the city, so the y alone is unambiguous.
    if (frames === 160) {
        const doors = tree.nodesInGroup("street_door");
        if (doors.length === 0) { report("door reachable", false, "no door"); return; }
        doorPos = doors[0].getPosition();
        player.setPosition(doorPos.x, doorPos.y, doorPos.z);
        player.setVelocity(0.0, 0.0, 0.0);
        return;
    }
    if (frames === 200) {
        const p = player.getPosition();
        report("door leads inside", p.y < INTERIOR_Y + 20.0,
               "y=" + p.y.toFixed(2));
        // And the room caught them: an interior floor is at its own origin, so
        // a player who fell through would keep going.
        report("interior floor holds", p.y > INTERIOR_Y - 2.0, "y=" + p.y.toFixed(2));
        console.log(ok ? "[E2E] PASS" : "[E2E] FAIL");
        tree.quit();
    }
}
