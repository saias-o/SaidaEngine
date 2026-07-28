// Coin or star. The Area reports the body; the autoload owns what it is worth.

exportProperty("kind", "coin");
exportProperty("value", 50);

let gameState = null;
let taken = false;

function onReady() {
    gameState = tree.autoload("GameState");
    if (gameState === null) throw new Error("GameState autoload is missing");

    node.on("bodyEntered", function (who) {
        if (taken || who !== "Player") return;
        taken = true;
        if (props.kind === "star") {
            audio.play("star");
            gameState.call("starTaken", props.value);
        } else if (props.kind === "relic") {
            audio.play("star");
            gameState.call("relicTaken", props.value);
        } else {
            audio.play("coin");
            gameState.call("coinTaken", props.value);
        }
        node.queueFree();
    });
}
