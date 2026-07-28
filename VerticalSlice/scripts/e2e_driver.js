// Headless check for the slice: waits for the scene to settle, then reports the
// pieces gameplay depends on and quits. Run with
//   SaidaEngine.exe --project VerticalSlice --play --test-autoload "E2E=scripts/e2e_driver.js"

let frames = 0;

function report(label, ok, detail) {
    console.log("[E2E] " + (ok ? "ok  " : "FAIL") + " " + label +
                (detail === undefined ? "" : "  " + detail));
    return ok;
}

function onUpdate() {
    frames += 1;

    // Deliberately does not fire: this pass asserts what the scene declares, and
    // a shot that lands would change the very counts being checked. The firing
    // path is covered by e2e_gameplay.js.
    if (frames !== 120) return;

    let ok = true;
    const player = tree.firstInGroup("player");
    const camera = tree.firstInGroup("camera");
    const gameState = tree.autoload("GameState");

    ok = report("player", player !== null) && ok;
    ok = report("camera", camera !== null) && ok;
    ok = report("autoload", gameState !== null) && ok;
    ok = report("physics", physics.available()) && ok;

    if (player !== null) {
        const p = player.getPosition();
        ok = report("player grounded", player.isOnFloor(),
                    "y=" + p.y.toFixed(2)) && ok;
        ok = report("player clip", player.currentClip() !== null,
                    String(player.currentClip())) && ok;
    }

    const counts = {
        enemy: tree.nodesInGroup("enemy").length,
        pickup: tree.nodesInGroup("pickup").length,
        target: tree.nodesInGroup("target").length,
        platform: tree.nodesInGroup("platform").length,
        impact: tree.nodesInGroup("impact").length,
        bolt: tree.nodesInGroup("tracer").length,
        fauna: tree.nodesInGroup("fauna").length,
    };
    ok = report("enemies", counts.enemy === 6, String(counts.enemy)) && ok;
    ok = report("pickups", counts.pickup === 10, String(counts.pickup)) && ok;
    ok = report("targets", counts.target === 3, String(counts.target)) && ok;
    ok = report("platforms", counts.platform === 3, String(counts.platform)) && ok;
    ok = report("effect pools", counts.impact === 6 && counts.bolt === 6,
                counts.impact + "/" + counts.bolt) && ok;
    ok = report("fauna", counts.fauna === 7, String(counts.fauna)) && ok;

    if (gameState !== null) {
        ok = report("phase", gameState.call("getPhase") === "grove",
                    String(gameState.call("getPhase"))) && ok;
        ok = report("health", gameState.call("getHealth") === 100,
                    String(gameState.call("getHealth"))) && ok;
    }

    const hud = tree.firstInGroup("hud_score");
    ok = report("hud", hud !== null && String(hud.getText()).indexOf("SCORE") === 0,
                hud === null ? "missing" : String(hud.getText())) && ok;

    console.log(ok ? "[E2E] PASS" : "[E2E] FAIL");
    tree.quit();
}
