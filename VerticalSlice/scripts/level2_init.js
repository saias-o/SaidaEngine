// Resets the scene-local progression stored by the persistent GameState
// autoload. This runs after The Deep Wilds hierarchy, including its HUD and
// dormant objective nodes, has entered the tree.

function onReady() {
    const gameState = tree.autoload("GameState");
    if (gameState === null) throw new Error("GameState autoload is missing");
    gameState.call("levelTwoReady");
}
