// Proves the level-one-to-level-two transition and the dense scene's gameplay
// contract. Run through Play mode so the persistent GameState autoload survives
// the scene replacement exactly as it does in the game.

let gameState = null;
let openingRequested = false;
let transitionRequested = false;
let levelTwoFrames = 0;

function report(label, ok, detail) {
    console.log("[E2E-L2] " + (ok ? "ok  " : "FAIL") + " " + label +
                (detail === undefined ? "" : "  " + detail));
    return ok;
}

function onReady() {
    gameState = tree.autoload("GameState");
}

function onUpdate() {
    const inLevelTwo = tree.firstInGroup("deep_wilds") !== null;
    if (!inLevelTwo) {
        const player = tree.firstInGroup("player");
        if (player === null) {
            if (!openingRequested) {
                openingRequested = true;
                tree.changeScene("scenes/verdance.scene");
            }
            return;
        }
        if (!transitionRequested) {
            transitionRequested = true;
            gameState.call("starTaken", 1000);
        }
        return;
    }

    levelTwoFrames += 1;
    if (levelTwoFrames !== 120) return;

    let ok = true;
    const player = tree.firstInGroup("player");
    const camera = tree.firstInGroup("camera");
    const vegetation = tree.nodesInGroup("wild_vegetation").length;
    const enemies = tree.nodesInGroup("enemy").length;
    const pickups = tree.nodesInGroup("pickup").length;
    const relics = tree.nodesInGroup("wild_relic").length;
    const hearts = tree.nodesInGroup("wild_heart").length;
    const fauna = tree.nodesInGroup("fauna").length;
    const impacts = tree.nodesInGroup("impact").length;
    const bolts = tree.nodesInGroup("tracer").length;

    ok = report("level marker", true) && ok;
    ok = report("player", player !== null) && ok;
    ok = report("camera", camera !== null) && ok;
    ok = report("player grounded", player !== null && player.isOnFloor()) && ok;
    ok = report("dense vegetation", vegetation >= 800,
                String(vegetation)) && ok;
    ok = report("guardians", enemies === 12, String(enemies)) && ok;
    ok = report("pickups", pickups === 22, String(pickups)) && ok;
    ok = report("dormant objectives", relics === 3 && hearts === 1,
                relics + "/" + hearts) && ok;
    ok = report("fauna", fauna === 12, String(fauna)) && ok;
    ok = report("effect pools", impacts === 10 && bolts === 10,
                impacts + "/" + bolts) && ok;
    ok = report("phase", gameState.call("getPhase") === "deep_wilds",
                String(gameState.call("getPhase"))) && ok;

    const objective = tree.firstInGroup("hud_objective");
    ok = report("HUD objective",
                objective !== null &&
                String(objective.getText()).indexOf("GUARDIANS") >= 0,
                objective === null ? "missing" : String(objective.getText())) && ok;

    for (let i = 0; i < 12; i += 1) gameState.call("enemyDown");
    ok = report("guardians unlock relics",
                gameState.call("getPhase") === "relics",
                String(gameState.call("getPhase"))) && ok;

    for (let i = 0; i < 3; i += 1) gameState.call("relicTaken", 500);
    ok = report("relics awaken heart",
                gameState.call("getPhase") === "heart",
                String(gameState.call("getPhase"))) && ok;

    gameState.call("starTaken", 2500);
    ok = report("heart completes run",
                gameState.call("getPhase") === "won",
                String(gameState.call("getPhase"))) && ok;

    console.log(ok ? "[E2E-L2] PASS" : "[E2E-L2] FAIL");
    tree.quit();
}
