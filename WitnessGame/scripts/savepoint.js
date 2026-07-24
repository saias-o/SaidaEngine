// Save point: the autoload is the sole owner of progress and its
// durable representation.

function onReady() {
    const gameState = tree.autoload("GameState");
    if (gameState === null) throw new Error("GameState autoload is missing");
    node.on("bodyEntered", function (who) {
        if (who !== "Player") return;
        const state = gameState.call("saveProgress");
        audio.play("save");
        console.log("[SavePoint] saved — relics=" + state.relics +
                    " saves=" + state.saves);
    });
}
