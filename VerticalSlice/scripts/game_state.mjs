// GameState — the run's single source of truth, and the only script that writes
// the HUD. Every other script reaches it with tree.autoload("GameState") and
// NodeRef.call(); nothing else knows the score, the health or the phase.

const SLOT = "verdance";

const TARGETS = 3;
const LEVEL_ONE_COINS = 9;
const LEVEL_ONE_ENEMIES = 6;
const LEVEL_TWO_COINS = 18;
const LEVEL_TWO_GUARDIANS = 12;
const LEVEL_TWO_RELICS = 3;

const PHASE_GROVE = "grove";      // shoot the three targets
const PHASE_CLIMB = "climb";      // cross the gauntlet
const PHASE_ARENA = "arena";      // clear the ruin
const PHASE_DEEP_WILDS = "deep_wilds";
const PHASE_RELICS = "relics";
const PHASE_HEART = "heart";
const PHASE_TRANSITION = "transition";
const PHASE_WON = "won";
const PHASE_LOST = "lost";

let level = 1;
let score = 0;
let health = 100;
let targetsHit = 0;
let coins = 0;
let kills = 0;
let relics = 0;
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
    if (hud !== null && hud.score.valid()) return hud;
    hud = null;
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
    if (phase === PHASE_GROVE) return "OBJECTIVE  Destroy the 3 targets  [" + targetsHit + "/" + TARGETS + "]";
    if (phase === PHASE_CLIMB) return "OBJECTIVE  Reach the ruins across the platforms";
    if (phase === PHASE_ARENA) return "OBJECTIVE  Clear the arena  [" + kills + "/" + LEVEL_ONE_ENEMIES + "]  then take the star";
    if (phase === PHASE_DEEP_WILDS) return "OBJECTIVE  Explore the sanctuaries  GUARDIANS [" + kills + "/" + LEVEL_TWO_GUARDIANS + "]";
    if (phase === PHASE_RELICS) return "OBJECTIVE  Awaken the 3 ancient relics  [" + relics + "/" + LEVEL_TWO_RELICS + "]";
    if (phase === PHASE_HEART) return "OBJECTIVE  Reach the ancient heart";
    if (phase === PHASE_TRANSITION) return "OBJECTIVE  Enter the Deep Wilds";
    if (phase === PHASE_WON) return "COMPLETE";
    return "DEFEAT";
}

function refresh() {
    const h = resolveHud();
    if (h === null) return;

    h.score.setText("SCORE  " + score + "        RECORD  " + Math.max(best, score));
    if (h.health !== null) {
        h.health.setText("HEALTH  " + bar(health, 100, 20) + "  " + Math.round(health) +
                         "%        COINS  " + coins + "/" +
                         (level === 1 ? LEVEL_ONE_COINS : LEVEL_TWO_COINS));
    }
    if (h.objective !== null) h.objective.setText(objectiveLine());
    if (h.banner !== null) {
        h.banner.setText(time.elapsed() < bannerUntil ? banner : "");
    }
    if (h.hint !== null) {
        h.hint.setText(phase === PHASE_WON || phase === PHASE_LOST
            ? "R  replay"
            : "WASD/ZQSD  move     SPACE  double jump     LEFT CLICK  fire     SHIFT  sprint     R  restart");
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
        say("THE BRIDGE IS OPEN — follow the coins", 3.5);
    } else {
        say("TARGET " + targetsHit + "/" + TARGETS, 1.2);
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
    if (kills >= LEVEL_ONE_ENEMIES && phase === PHASE_ARENA) {
        say("ARENA CLEARED — take the star", 3.5);
    } else if (kills >= LEVEL_TWO_GUARDIANS && phase === PHASE_DEEP_WILDS) {
        phase = PHASE_RELICS;
        const dormantRelics = tree.nodesInGroup("wild_relic");
        for (let i = 0; i < dormantRelics.length; i += 1) {
            dormantRelics[i].setEnabled(true);
        }
        audio.play("victory");
        say("THE RELICS ANSWER — search the three sanctuaries", 4.0);
    }
    refresh();
    return kills;
}

export function starTaken(value) {
    stars += 1;
    score += Number(value) || 0;
    if (phase === PHASE_LOST) return stars;

    if (level === 1) {
        phase = PHASE_TRANSITION;
        audio.play("victory");
        say("THE DEEP WILDS OPEN", 2.0);
        hud = null;
        tree.changeScene("scenes/deep_wilds.scene");
    } else {
        phase = PHASE_WON;
        audio.play("victory");
        say("THE HEART LIVES AGAIN!   score " + score, 999);
        persistBest();
    }
    refresh();
    return stars;
}

export function relicTaken(value) {
    if (phase !== PHASE_RELICS) return relics;
    relics += 1;
    score += Number(value) || 0;
    if (relics >= LEVEL_TWO_RELICS) {
        phase = PHASE_HEART;
        const heart = tree.firstInGroup("wild_heart");
        if (heart !== null) heart.setEnabled(true);
        audio.play("victory");
        say("THE ANCIENT HEART AWAKENS", 4.0);
    } else {
        say("RELIC " + relics + "/" + LEVEL_TWO_RELICS, 1.8);
    }
    refresh();
    return relics;
}

export function levelTwoReady() {
    level = 2;
    health = Math.max(health, 75);
    coins = 0;
    kills = 0;
    relics = 0;
    phase = PHASE_DEEP_WILDS;
    hud = null;
    say("THE DEEP WILDS — FIND THE SANCTUARIES", 4.5);
    return true;
}

// Called by the arena trigger the first time the player sets foot up there.
export function enterArena() {
    if (phase !== PHASE_CLIMB) return phase;
    phase = PHASE_ARENA;
    say("THEY AWAKEN", 3.0);
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
        say("YOU HAVE FALLEN — press R to replay", 999);
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
