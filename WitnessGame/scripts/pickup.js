// Pickup relic. Shared state belongs to the GameState autoload.

exportProperty("points", 1);

let collected = false;

function onReady() {
    const gameState = tree.autoload("GameState");
    if (gameState === null) throw new Error("GameState autoload is missing");
    node.on("bodyEntered", function (who) {
        if (collected || who !== "Player") return;
        collected = true;
        audio.play("pickup");
        const total = gameState.call("addRelics", props.points);
        console.log("[Pickup] " + node.getName() + " collected — relics=" + total);
        node.queueFree();
    });
}
