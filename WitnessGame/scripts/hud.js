// HUD attached to ScoreText. It reads the GameState autoload from a
// different QuickJS context; only the autoload knows about durable storage.

let last = null;
let gameState = null;

function refresh() {
    const relics = Number(gameState.call("getRelics")) || 0;
    if (relics === last) return;
    last = relics;
    node.setText("Relics: " + relics);
}

function onReady() {
    gameState = tree.autoload("GameState");
    if (gameState === null) throw new Error("GameState autoload is missing");
    refresh();
    time.every(0.25, refresh);
}
