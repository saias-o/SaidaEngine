// Adaptive prompt attached to PromptText: the movement binding label
// follows the last active device (input.lastActiveDevice). Before any
// activity ("none"), the keyboard prompt is the default.

const LABELS = {
    "keyboard-mouse": "Move: WASD",
    "gamepad": "Move: Left Stick",
    "touch": "Move: Swipe",
};
const DEFAULT_LABEL = LABELS["keyboard-mouse"];

let last = null;

function refresh() {
    const device = String(input.lastActiveDevice());
    if (device === last) return;
    last = device;
    node.setText(LABELS[device] || DEFAULT_LABEL);
}

function onReady() {
    refresh();
    time.every(0.1, refresh);
}
