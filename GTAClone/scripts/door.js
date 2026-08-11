// A doorway between the street and an interior cell.
//
// Interiors are built as sealed rooms well below the city rather than carved out
// of the building models, which are solid: entering one is a teleport, the way
// the games this is modelled on did it. The volume is one-way — it moves the
// player to its destination and then ignores them until they have left it, so
// standing in a doorway does not bounce them back and forth.

exportProperty("toX", 0.0);
exportProperty("toY", 0.0);
exportProperty("toZ", 0.0);

let inside = false;

function onReady() {
    node.on("bodyEntered", function (who) {
        if (who !== "Player" || inside) return;
        const player = tree.firstInGroup("player");
        if (player === null) return;
        inside = true;
        player.setPosition(props.toX, props.toY, props.toZ);
        // The character keeps its momentum across the move, which would carry it
        // straight back through the door it arrived in.
        player.setVelocity(0.0, 0.0, 0.0);
    });
    node.on("bodyExited", function (who) {
        if (who === "Player") inside = false;
    });
}
