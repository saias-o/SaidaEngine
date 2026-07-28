// A one-shot volume: the first time the player enters it, one named function on
// the autoload is called. Keeps level progression in the scene's layout rather
// than in a distance check somewhere else.

exportProperty("call", "");

let gameState = null;
let fired = false;

function onReady() {
    gameState = tree.autoload("GameState");
    if (gameState === null) throw new Error("GameState autoload is missing");

    node.on("bodyEntered", function (who) {
        if (fired || who !== "Player" || props.call === "") return;
        fired = true;
        gameState.call(props.call);
    });
}
