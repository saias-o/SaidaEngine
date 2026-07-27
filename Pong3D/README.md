# Pong 3D

A one-scene game built on SaidaEngine without touching the engine: a `.saidaproj`,
one `.scene` and one QuickJS script.

## Run

```sh
./build/bin/SaidaEngine.exe --project Pong3D --play
```

The editor's Open Project dialog scans `SAIDA_PROJECT_ROOT` (the engine
checkout), so `Pong3D` also appears in the list.

## Controls

| Action | Keyboard | Gamepad |
|---|---|---|
| Move the paddle | `A`/`D` or `Left`/`Right` | left stick X |
| Serve | `Space` or `Enter` | `A` |
| Restart the match | `R` | `Start` |

First to 7 points wins.

## How it is put together

`scenes/pong.scene` holds only data: the table, the two side walls, the goal
lines, the paddles, the ball (with a `Rotator` for spin and a `ParticleSystem`
trail), the camera and the HUD text nodes.

`scripts/pong.js` is attached to the `GameController` node and runs the whole
match. It reaches the ball, the paddles and the HUD through the scene's groups
(`ball`, `paddle_player`, `paddle_ai`, `pong_score`, `pong_message`), so no node
id is hard-coded. Every tunable — field size, speeds, AI slack, win score — is an
`exportProperty` overridden from the scene's `properties` block, and can be
edited in the Inspector.

The ball is integrated by the script rather than by Jolt: paddles and ball are
plain transform nodes, collision is a swept test against the paddle planes, and
the frame step is clamped so a long frame cannot tunnel the ball through a
paddle. The result is a deterministic rally that does not depend on physics
tuning.

`scripts/pong.js` calls `input.applyProfile` in `onReady`. That call *replaces*
the engine's default bindings, so the profile restates the standard `Move*`
actions alongside the game's own.

## Test

`scripts/e2e_driver.js` is never referenced by the game; it is loaded as an
autoload by the harness and drives the match through injected actions:

```sh
./build/bin/SaidaEngine.exe --project Pong3D --play --test-autoload PongDriver=scripts/e2e_driver.js
```

It checks the HUD, the paddle's response to input, the serve, and that the AI
returns the first rally, then logs `[E2E] PASS: ...` and quits.
