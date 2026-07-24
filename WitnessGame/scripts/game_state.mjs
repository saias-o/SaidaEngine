// GameState — persistent autoload for the witness game.
// Other scripts reach it via tree.autoload("GameState") then
// NodeRef.call(); they never touch the save slot directly.

const SLOT = "witness";

function readState() {
    const raw = storage.load(SLOT);
    if (raw === null) return { relics: 0, saves: 0 };
    try {
        const parsed = JSON.parse(raw);
        return {
            relics: Number(parsed.relics) || 0,
            saves: Number(parsed.saves) || 0,
        };
    } catch (_) {
        return { relics: 0, saves: 0 };
    }
}

let state = readState();

function persist() {
    return storage.save(SLOT, JSON.stringify(state));
}

export function getState() {
    return { relics: state.relics, saves: state.saves };
}

export function getRelics() {
    return state.relics;
}

export function addRelics(amount) {
    state.relics += Number(amount) || 0;
    persist();
    return state.relics;
}

export function saveProgress() {
    state.saves += 1;
    persist();
    return getState();
}

export function reset() {
    state = { relics: 0, saves: 0 };
    storage.remove(SLOT);
    persist();
    return getState();
}

export function onReady() {
    if (!storage.has(SLOT)) persist();
    console.log(
        "[GameState] ready — relics=" + state.relics + " saves=" + state.saves);
}
