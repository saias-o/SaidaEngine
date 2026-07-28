// Plays the opening of the slice and asserts the loop actually closes: the shot
// reaches a target, the target reports itself, the score moves and the phase
// advances. Structure alone (e2e_driver.js) cannot catch a weapon that fires
// into nothing.
//
//   SaidaEngine.exe --project VerticalSlice --play \
//       --test-autoload "E2E=scripts/e2e_gameplay.js"

let frames = 0;
let gameState = null;
let startScore = 0;
let targetsAtStart = 0;
let gameplayRequested = false;

function report(label, ok, detail) {
    console.log("[E2E] " + (ok ? "ok  " : "FAIL") + " " + label +
                (detail === undefined ? "" : "  " + detail));
    return ok;
}

function onReady() {
    gameState = tree.autoload("GameState");
}

function onUpdate() {
    if (tree.firstInGroup("player") === null) {
        if (!gameplayRequested) {
            gameplayRequested = true;
            tree.changeScene("scenes/verdance.scene");
        }
        return;
    }
    frames += 1;

    if (frames === 20) {
        targetsAtStart = tree.nodesInGroup("target").length;
        startScore = Number(gameState.call("addScore", 0)) || 0;
    }

    // Hold the trigger through the middle of the run: the targets stand on the
    // firing line straight ahead of the spawn.
    if (frames > 40 && frames < 220) input.inject("Fire", true);
    if (frames === 220) input.inject("Fire", false);

    if (frames !== 280) return;

    let ok = true;
    const player = tree.firstInGroup("player");
    const p = player === null ? null : player.getPosition();

    ok = report("player alive", player !== null && p.y > -20.0,
                p === null ? "missing" : p.y.toFixed(2)) && ok;

    const remaining = tree.nodesInGroup("target").length;
    ok = report("a target went down", remaining < targetsAtStart,
                targetsAtStart + " -> " + remaining) && ok;

    const score = Number(gameState.call("addScore", 0)) || 0;
    ok = report("score moved", score > startScore, startScore + " -> " + score) && ok;

    ok = report("health intact", Number(gameState.call("getHealth")) === 100,
                String(gameState.call("getHealth"))) && ok;

    const hud = tree.firstInGroup("hud_score");
    ok = report("hud shows the score",
                hud !== null && String(hud.getText()).indexOf("SCORE  " + score) === 0,
                hud === null ? "missing" : String(hud.getText())) && ok;

    console.log(ok ? "[E2E] GAMEPLAY PASS" : "[E2E] GAMEPLAY FAIL");
    tree.quit();
}
