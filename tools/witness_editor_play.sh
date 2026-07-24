#!/usr/bin/env bash
# E2E for the EDITOR's Play mode: copies WitnessGame, injects the driver as an
# ephemeral autoload without rewriting the project, triggers the real
# transition into Play via --play, then verifies gameplay, HUD, and restoration
# on the second launch. Requires a machine with a GPU (the editor opens a
# window).
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

PROJECT="${1:-$ROOT/build/witness-editor-play}"
LOG="$ROOT/build/editor_play.log"
RESTART_LOG="$ROOT/build/editor_play_restart.log"
rm -rf "$PROJECT"
cp -R WitnessGame "$PROJECT"
# The source copy may contain local saves ignored by Git. The first launch
# must always start from a clean state; only the second run reuses the
# progress produced in this test copy.
rm -rf "$PROJECT/saves"

./build/bin/SaidaEngine.exe --project "$PROJECT/WitnessGame.saidaproj" \
    --play --test-autoload "E2EDriver=scripts/e2e_driver.js" > "$LOG" 2>&1 || true

if ! grep -q "\[E2E\] PASS" "$LOG"; then
    echo "WITNESS EDITOR-PLAY E2E: FAIL"
    grep -E "\[E2E\]|\[JS\]|error" "$LOG" | tail -20
    exit 1
fi

./build/bin/SaidaEngine.exe --project "$PROJECT/WitnessGame.saidaproj" \
    --play --test-autoload "E2EDriver=scripts/e2e_driver.js" \
    > "$RESTART_LOG" 2>&1 || true

if ! grep -q "\[E2E\] RESTART PASS" "$RESTART_LOG"; then
    echo "WITNESS EDITOR-PLAY E2E: FAIL (restart)"
    grep -E "\[E2E\]|\[JS\]|error" "$RESTART_LOG" | tail -20
    exit 1
fi

echo "WITNESS EDITOR-PLAY E2E: PASS (run + restart, UI included)"
