// GameState — the run's single source of truth, and the only script that writes
// the HUD. Every other script reaches it with tree.autoload("GameState") and
// NodeRef.call(); nothing else knows the score, the health or the phase.

const SLOT = "verdance";

const TARGETS = 3;
const COINS = 9;
const ENEMIES = 6;

const PHASE_GROVE = "grove";      // shoot the three targets
const PHASE_CLIMB = "climb";      // cross the gauntlet
const PHASE_ARENA = "arena";      // clear the ruin
const PHASE_WON = "won";
const PHASE_LOST = "lost";

let score = 0;
let health = 100;
let targetsHit = 0;
let coins = 0;
let kills = 0;
let stars = 0;
let phase = PHASE_GROVE;
let banner = "";
let bannerUntil = 0;
let best = 0;

let hud = null;

function readBest() {
    const raw = storage.load(SLOT);
    if (raw === null) return 0;
    try {
        return Number(JSON.parse(raw).best) || 0;
    } catch (_) {
        return 0;
    }
}

function resolveHud() {
    if (hud !== null) return hud;
    const scoreText = tree.firstInGroup("hud_score");
    if (scoreText === null) return null;
    hud = {
        score: scoreText,
        health: tree.firstInGroup("hud_health"),
        objective: tree.firstInGroup("hud_objective"),
        banner: tree.firstInGroup("hud_banner"),
        hint: tree.firstInGroup("hud_hint"),
    };
    return hud;
}

function bar(value, max, width) {
    const filled = Math.max(0, Math.min(width, Math.round((value / max) * width)));
    let out = "";
    for (let i = 0; i < width; i += 1) out += i < filled ? "|" : ".";
    return out;
}

function objectiveLine() {
    if (phase === PHASE_GROVE) return "OBJECTIF  Detruire les 3 cibles  [" + targetsHit + "/" + TARGETS + "]";
    if (phase === PHASE_CLIMB) return "OBJECTIF  Rejoindre les ruines par les plateformes";
    if (phase === PHASE_ARENA) return "OBJECTIF  Nettoyer l'arene  [" + kills + "/" + ENEMIES + "]  puis prendre l'etoile";
    if (phase === PHASE_WON) return "TERMINE";
    return "PERDU";
}

function refresh() {
    const h = resolveHud();
    if (h === null) return;

    h.score.setText("SCORE  " + score + "        RECORD  " + Math.max(best, score));
    if (h.health !== null) {
        h.health.setText("VIE  " + bar(health, 100, 20) + "  " + Math.round(health) +
                         "%        PIECES  " + coins + "/" + COINS);
    }
    if (h.objective !== null) h.objective.setText(objectiveLine());
    if (h.banner !== null) {
        h.banner.setText(time.elapsed() < bannerUntil ? banner : "");
    }
    if (h.hint !== null) {
        h.hint.setText(phase === PHASE_WON || phase === PHASE_LOST
            ? "R  rejouer"
            : "ZQSD/WASD  bouger     ESPACE  sauter (x2)     CLIC GAUCHE  tirer     MAJ  courir     R  recommencer");
    }
}

function say(text, seconds) {
    banner = text;
    bannerUntil = time.elapsed() + (seconds || 2.5);
    refresh();
}

export function getPhase() {
    return phase;
}

export function getHealth() {
    return health;
}

export function isOver() {
    return phase === PHASE_WON || phase === PHASE_LOST;
}

export function addScore(amount) {
    score += Number(amount) || 0;
    refresh();
    return score;
}

export function targetDown() {
    targetsHit += 1;
    score += 100;
    if (targetsHit >= TARGETS && phase === PHASE_GROVE) {
        phase = PHASE_CLIMB;
        audio.play("victory");
        say("LE PONT EST OUVERT — suivez les pieces", 3.5);
    } else {
        say("CIBLE " + targetsHit + "/" + TARGETS, 1.2);
    }
    refresh();
    return targetsHit;
}

export function coinTaken(value) {
    coins += 1;
    score += Number(value) || 0;
    refresh();
    return coins;
}

export function enemyDown() {
    kills += 1;
    score += 250;
    if (kills >= ENEMIES && phase === PHASE_ARENA) {
        say("ARENE NETTOYEE — prenez l'etoile", 3.5);
    }
    refresh();
    return kills;
}

export function starTaken(value) {
    stars += 1;
    score += Number(value) || 0;
    if (phase !== PHASE_LOST) {
        phase = PHASE_WON;
        audio.play("victory");
        say("VICTOIRE !   score " + score, 999);
        persistBest();
    }
    refresh();
    return stars;
}

// Called by the arena trigger the first time the player sets foot up there.
export function enterArena() {
    if (phase !== PHASE_CLIMB) return phase;
    phase = PHASE_ARENA;
    say("ILS SE REVEILLENT", 3.0);
    refresh();
    return phase;
}

export function damagePlayer(amount) {
    if (phase === PHASE_WON || phase === PHASE_LOST) return health;
    health -= Number(amount) || 0;
    if (health <= 0) {
        health = 0;
        phase = PHASE_LOST;
        audio.play("defeat");
        say("VOUS ETES TOMBE — R pour rejouer", 999);
        persistBest();
    }
    refresh();
    return health;
}

export function healPlayer(amount) {
    if (phase === PHASE_LOST) return health;
    health = Math.min(100, health + (Number(amount) || 0));
    refresh();
    return health;
}

export function announce(text, seconds) {
    say(String(text), Number(seconds) || 2.5);
    return true;
}

function persistBest() {
    if (score > best) best = score;
    storage.save(SLOT, JSON.stringify({ best: best }));
}

export function onReady() {
    best = readBest();
    // The HUD belongs to the scene, which is not up yet when an autoload runs.
    time.every(0.2, refresh);
    console.log("[Verdance] GameState ready — best=" + best);
}
