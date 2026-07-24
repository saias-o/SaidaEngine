#!/usr/bin/env bash
# E2E for the "ship" path: exports WitnessGame with the REAL BuildExporter
# (saida_tool export-game — the same code path as the Build button), launches
# the standalone runtime with an ephemeral test autoload, and greps the
# verdict. The exported artefact is never rewritten. Requires a machine with a
# GPU.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

OUT="${1:-$ROOT/build/witness-e2e}"
rm -rf "$OUT"

./build/bin/saida_tool.exe export-game WitnessGame/WitnessGame.saidaproj \
    --out "$OUT" --version 1.0.0 --company Saida > /dev/null

cd "$OUT"
# Packaged games persist their saves under the OS user folder. Pin them to a
# fresh folder per invocation (shared by both runs): the restart proof stays
# hermetic, unaffected by saves from a previous run in
# %APPDATA%/~/.local/share.
export SAIDA_SAVE_DIR="$OUT/.saves"

"./Witness Game.exe" --test-autoload \
    "E2EDriver=scripts/e2e_driver.js" > e2e.log 2>&1 || true

if ! grep -q "\[E2E\] PASS" e2e.log; then
    echo "WITNESS E2E: FAIL"
    grep -E "\[E2E\]|\[JS\]|error" e2e.log | tail -20
    exit 1
fi

# Second launch, same folder: the progress saved by run 1 must be restored
# from saves/ at boot (save/load after restart).
"./Witness Game.exe" --test-autoload \
    "E2EDriver=scripts/e2e_driver.js" > e2e_restart.log 2>&1 || true

if ! grep -q "\[E2E\] RESTART PASS" e2e_restart.log; then
    echo "WITNESS E2E: FAIL (restart: progress not restored)"
    grep -E "\[E2E\]|\[JS\]|error" e2e_restart.log | tail -20
    exit 1
fi

echo "WITNESS E2E: PASS (run + restart)"
exit 0
