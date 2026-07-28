// A practice target: takes the player's shot, flashes, and reports itself once.

exportProperty("value", 100);
exportProperty("index", 0);

let gameState = null;
let burst = null;
let broken = false;

// Called from the player's shot, across contexts.
function takeDamage(amount) {
    if (broken) return 0;
    broken = true;
    audio.play("target_break");
    if (burst !== null) burst.setEnabled(true);
    if (gameState !== null) gameState.call("targetDown");
    // The burst is a child of this body, so it dies with it: one timer, and
    // nothing touches the burst after the free.
    time.wait(0.7, function () { node.queueFree(); });
    return Number(amount) || 0;
}

function onReady() {
    gameState = tree.autoload("GameState");
    burst = tree.firstInGroup("burst_" + node.getName());
    if (burst !== null) burst.setEnabled(false);
}
