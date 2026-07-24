// Scene-change door. Attached to an Area node: when the player
// enters, it switches the sub-scene (the World and autoloads survive).

exportProperty("targetScene", "scenes/hub.scene");

let used = false;

function onReady() {
    node.on("bodyEntered", function (who) {
        if (used || who !== "Player") return;
        used = true;  // changeScene is deferred: don't trigger it twice
        console.log("[Door] -> " + props.targetScene);
        tree.changeScene(props.targetScene);
    });
}
