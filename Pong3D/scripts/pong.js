// Pong 3D — the whole game runs from this single script.
//
// It owns the ball, the two paddles and the HUD, which it reaches through the
// groups declared in scenes/pong.scene. Nothing here needs a native behaviour:
// the nodes are plain transforms and the script integrates the ball itself, so
// the match stays deterministic instead of depending on physics tuning.

exportProperty("halfWidth", 9.0);            // playfield half-width (wall inner face)
exportProperty("halfLength", 13.0);          // half-length, goal line to goal line
exportProperty("paddleZ", 11.0);             // |z| of both paddles
exportProperty("paddleHalfLength", 1.6);
exportProperty("paddleHalfThickness", 0.3);
exportProperty("ballRadius", 0.35);
exportProperty("ballHeight", 0.45);
exportProperty("playerSpeed", 16.0);
exportProperty("aiSpeed", 10.5);
exportProperty("aiSlack", 0.55);             // dead zone that keeps the AI beatable
exportProperty("serveSpeed", 12.0);
exportProperty("maxSpeed", 28.0);
exportProperty("speedUp", 1.05);             // per successful return
exportProperty("spin", 9.0);                 // sideways kick from an off-centre hit
exportProperty("winScore", 7);

// The profile REPLACES the engine defaults (Input::applyBindingProfile), so the
// standard movement actions are restated here rather than lost.
const INPUT_PROFILE = {
    schema: 1,
    name: "pong3d",
    bindings: [
        { action: "PaddleLeft", context: "Global", device: "keyboard", control: "A" },
        { action: "PaddleLeft", context: "Global", device: "keyboard", control: "Left" },
        { action: "PaddleRight", context: "Global", device: "keyboard", control: "D" },
        { action: "PaddleRight", context: "Global", device: "keyboard", control: "Right" },
        { action: "Serve", context: "Global", device: "keyboard", control: "Space" },
        { action: "Serve", context: "Global", device: "keyboard", control: "Enter" },
        { action: "Restart", context: "Global", device: "keyboard", control: "R" },
        { action: "PaddleLeft", context: "Global", device: "gamepad-axis", control: "LeftX", scale: -1.0 },
        { action: "PaddleRight", context: "Global", device: "gamepad-axis", control: "LeftX", scale: 1.0 },
        { action: "Serve", context: "Global", device: "gamepad-button", control: "A" },
        { action: "Restart", context: "Global", device: "gamepad-button", control: "Start" },
        { action: "MoveForward", context: "Global", device: "keyboard", control: "W" },
        { action: "MoveBackward", context: "Global", device: "keyboard", control: "S" },
        { action: "MoveLeft", context: "Global", device: "keyboard", control: "A" },
        { action: "MoveRight", context: "Global", device: "keyboard", control: "D" }
    ]
};

const STATE_SERVE = "serve";
const STATE_PLAY = "play";
const STATE_OVER = "over";

let ball = null;
let playerPaddle = null;
let aiPaddle = null;
let scoreText = null;
let messageText = null;

let ballX = 0.0;
let ballZ = 0.0;
let velX = 0.0;
let velZ = 0.0;
let playerX = 0.0;
let aiX = 0.0;
let aiTargetX = 0.0;

let playerScore = 0;
let aiScore = 0;
let state = STATE_SERVE;
let serveToward = -1.0;      // -1 = toward the AI, +1 = toward the player
let lastScoreLabel = null;
let lastMessage = null;

function clamp(value, low, high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

function requireGroup(group) {
    const found = tree.firstInGroup(group);
    if (found === null) throw new Error("Pong: no node in group '" + group + "'");
    return found;
}

function paddleLimit() {
    return props.halfWidth - props.paddleHalfLength;
}

function setMessage(text) {
    if (text === lastMessage) return;
    lastMessage = text;
    messageText.setText(text);
}

function refreshScore() {
    const label = "YOU " + playerScore + "   -   AI " + aiScore;
    if (label === lastScoreLabel) return;
    lastScoreLabel = label;
    scoreText.setText(label);
}

// Parks the ball at the centre and waits for a serve. `toward` is the side the
// next serve flies to, so whoever conceded gets the ball played away from them.
function resetRally(toward) {
    serveToward = toward;
    state = STATE_SERVE;
    ballX = 0.0;
    ballZ = 0.0;
    velX = 0.0;
    velZ = 0.0;
    setMessage("Press Space to serve.");
}

function restartMatch() {
    playerScore = 0;
    aiScore = 0;
    playerX = 0.0;
    aiX = 0.0;
    aiTargetX = 0.0;
    refreshScore();
    resetRally(-1.0);
}

function serveBall() {
    // A shallow angle keeps the first exchange readable; the rally sharpens it.
    const spread = (Math.random() * 2.0 - 1.0) * 0.45;
    const length = Math.sqrt(spread * spread + 1.0);
    velX = (spread / length) * props.serveSpeed;
    velZ = (serveToward / length) * props.serveSpeed;
    state = STATE_PLAY;
    setMessage("");
}

function score(playerScored) {
    if (playerScored) playerScore += 1;
    else aiScore += 1;
    refreshScore();

    if (playerScore >= props.winScore || aiScore >= props.winScore) {
        state = STATE_OVER;
        ballX = 0.0;
        ballZ = 0.0;
        velX = 0.0;
        velZ = 0.0;
        setMessage(playerScore > aiScore
            ? "You win " + playerScore + "-" + aiScore + ". Press R to play again."
            : "AI wins " + aiScore + "-" + playerScore + ". Press R to play again.");
        return;
    }
    // Serve away from whoever just conceded.
    resetRally(playerScored ? -1.0 : 1.0);
}

// Reflects the ball off a paddle and returns true when it connected. `plane` is
// the z the ball centre may not cross, `sign` the direction it must travel in.
function bounceOffPaddle(paddleX, plane, sign, previousZ, nextZ, dt) {
    const crossed = sign < 0.0 ? (previousZ >= plane && nextZ <= plane)
                               : (previousZ <= plane && nextZ >= plane);
    if (!crossed) return false;

    // Ball x at the moment it reaches the plane, so a fast ball is not judged
    // against a position it only reaches after the paddle.
    const span = nextZ - previousZ;
    const ratio = Math.abs(span) < 1e-6 ? 0.0 : (plane - previousZ) / span;
    const hitX = ballX - velX * dt * (1.0 - ratio);
    const reach = props.paddleHalfLength + props.ballRadius;
    const offset = hitX - paddleX;
    if (Math.abs(offset) > reach) return false;

    ballX = hitX;
    ballZ = plane;
    velZ = -velZ;
    velX += (offset / reach) * props.spin;

    const speed = Math.sqrt(velX * velX + velZ * velZ);
    const target = Math.min(speed * props.speedUp, props.maxSpeed);
    velX = (velX / speed) * target;
    velZ = (velZ / speed) * target;

    // A return that is nearly sideways never comes back; keep some depth.
    const minZ = target * 0.45;
    if (Math.abs(velZ) < minZ) {
        velZ = velZ < 0.0 ? -minZ : minZ;
        const rescale = Math.sqrt(Math.max(target * target - velZ * velZ, 0.0));
        velX = velX < 0.0 ? -rescale : rescale;
    }
    return true;
}

function updatePlayer(dt) {
    const direction = input.axis("PaddleLeft", "PaddleRight");
    playerX = clamp(playerX + direction * props.playerSpeed * dt,
                    -paddleLimit(), paddleLimit());
}

function updateAI(dt) {
    if (state === STATE_PLAY && velZ < 0.0) {
        aiTargetX = ballX;                 // the ball is coming: track it
    } else {
        aiTargetX = ballX * 0.25;          // otherwise drift back toward the middle
    }

    const delta = aiTargetX - aiX;
    if (Math.abs(delta) <= props.aiSlack) return;
    const step = props.aiSpeed * dt;
    const move = clamp(delta, -step, step);
    aiX = clamp(aiX + move, -paddleLimit(), paddleLimit());
}

function updateBall(dt) {
    const previousZ = ballZ;
    ballX += velX * dt;
    ballZ += velZ * dt;

    // Side walls.
    const limit = props.halfWidth - props.ballRadius;
    if (ballX < -limit) {
        ballX = -limit - (ballX + limit);
        velX = -velX;
    } else if (ballX > limit) {
        ballX = limit - (ballX - limit);
        velX = -velX;
    }

    const gap = props.paddleHalfThickness + props.ballRadius;
    if (velZ > 0.0) {
        bounceOffPaddle(playerX, props.paddleZ - gap, 1.0, previousZ, ballZ, dt);
    } else if (velZ < 0.0) {
        bounceOffPaddle(aiX, -props.paddleZ + gap, -1.0, previousZ, ballZ, dt);
    }

    if (ballZ > props.halfLength) score(false);
    else if (ballZ < -props.halfLength) score(true);
}

function onReady() {
    input.applyProfile(JSON.stringify(INPUT_PROFILE));

    ball = requireGroup("ball");
    playerPaddle = requireGroup("paddle_player");
    aiPaddle = requireGroup("paddle_ai");
    scoreText = requireGroup("pong_score");
    messageText = requireGroup("pong_message");

    restartMatch();
    console.log("[Pong3D] ready — first to " + props.winScore + " wins.");
}

function onUpdate(dt) {
    // A long frame (window drag, shader compile) must not teleport the ball
    // through a paddle.
    const step = dt > 0.05 ? 0.05 : dt;

    if (state === STATE_OVER) {
        if (input.justPressed("Restart")) restartMatch();
    } else {
        updatePlayer(step);
        updateAI(step);
        if (state === STATE_SERVE) {
            if (input.justPressed("Serve")) serveBall();
        } else {
            updateBall(step);
        }
        if (input.justPressed("Restart")) restartMatch();
    }

    // A serving ball rides the serving paddle so the serve is never blind.
    if (state === STATE_SERVE) {
        ballX = serveToward < 0.0 ? playerX : aiX;
        ballZ = serveToward < 0.0 ? props.paddleZ - 1.2 : -props.paddleZ + 1.2;
    }

    // Purely cosmetic hop: the simulation stays flat in the XZ plane.
    const hop = state === STATE_PLAY
        ? Math.abs(Math.sin(time.elapsed() * 7.0)) * 0.18
        : 0.0;

    playerPaddle.setPosition(playerX, 0.35, props.paddleZ);
    aiPaddle.setPosition(aiX, 0.35, -props.paddleZ);
    ball.setPosition(ballX, props.ballHeight + hop, ballZ);
}
