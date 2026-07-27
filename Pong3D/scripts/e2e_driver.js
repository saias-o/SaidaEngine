// E2E driver for Pong 3D. The game never references it: the harness adds it as
// an autoload (`--test-autoload PongDriver=scripts/e2e_driver.js`), so it drives
// the match through injected actions only.
//
// It proves the four things the scene and pong.js are supposed to do together:
// the HUD exists, the paddle answers input, a serve puts the ball in motion,
// and the AI returns it (the ball crosses back toward the player).
// PASS/FAIL goes to the log as `[E2E] ...`; the exit code stays 0.

const TIMEOUT = 20.0;

let t = 0;
let finished = false;
let phase = 0;
let paddleStartX = 0;
let ballAfterServeZ = 0;
let reachedAISide = false;

function finish(verdict) {
    if (finished) return;
    finished = true;
    input.inject("Serve", 0);
    input.inject("PaddleRight", 0);
    console.log("[E2E] " + verdict);
    tree.quit();
}

function group(name) {
    return tree.firstInGroup(name);
}

function onReady() {
    console.log("[E2E] pong driver armed");
}

function onUpdate(dt) {
    if (finished) return;
    t += dt;
    if (t > TIMEOUT) return finish("FAIL: timeout in phase " + phase);

    const ball = group("ball");
    const paddle = group("paddle_player");
    const score = group("pong_score");
    const message = group("pong_message");
    if (ball === null || paddle === null || score === null || message === null) {
        if (t < 1.0) return;               // the scene may still be loading
        return finish("FAIL: pong nodes missing (scene not loaded?)");
    }

    // Phase 0 — the HUD is up and the game is waiting for a serve.
    if (phase === 0) {
        if (t < 0.5) return;
        if (String(score.getText()).indexOf("YOU 0") !== 0)
            return finish("FAIL: unexpected initial score '" + score.getText() + "'");
        if (String(message.getText()).indexOf("serve") < 0)
            return finish("FAIL: no serve prompt, got '" + message.getText() + "'");
        paddleStartX = paddle.getPosition().x;
        input.inject("PaddleRight", 1);
        phase = 1;
        return;
    }

    // Phase 1 — the paddle follows the injected axis.
    if (phase === 1) {
        if (t < 1.2) return;
        input.inject("PaddleRight", 0);
        const moved = paddle.getPosition().x - paddleStartX;
        if (moved < 1.0)
            return finish("FAIL: paddle did not move right (dx=" + moved + ")");
        input.inject("Serve", 1);
        phase = 2;
        return;
    }

    // Phase 2 — the serve launches the ball toward the AI.
    if (phase === 2) {
        input.inject("Serve", 0);
        if (t < 2.0) return;
        ballAfterServeZ = ball.getPosition().z;
        if (String(message.getText()) !== "")
            return finish("FAIL: serve prompt still shown after serving");
        if (ballAfterServeZ > 6.0)
            return finish("FAIL: ball did not leave the player side (z=" + ballAfterServeZ + ")");
        phase = 3;
        return;
    }

    // Phase 3 — the ball reaches the AI side, then comes back: the AI returned
    // it instead of conceding.
    if (phase === 3) {
        const z = ball.getPosition().z;
        if (!reachedAISide) {
            if (z < -6.0) reachedAISide = true;
            return;
        }
        if (String(score.getText()).indexOf("YOU 0") !== 0)
            return finish("FAIL: a point was conceded before the first return");
        if (z > 2.0) return finish("PASS: serve, paddle, wall and AI return all work");
    }
}
