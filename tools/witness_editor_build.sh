#!/usr/bin/env bash
# E2E for the EDITOR's Build button click: launches SaidaEngine.exe --build
# (exactly the dialog's Build button code path, not saida_tool), runs the
# exact artefact with an ephemeral test autoload, and greps the verdict.
# Requires a machine with a GPU (the editor creates its window).
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

OUT="${1:-$ROOT/build/witness-editor-build}"
case "$OUT" in
    /* | [A-Za-z]:[\\/]* ) ;;
    * ) OUT="$ROOT/$OUT" ;;
esac
RUN_OUT="$ROOT/build/witness-editor-build-run"
rm -rf "$OUT"
rm -rf "$RUN_OUT"
rm -rf "$ROOT/build/witness-editor-build-saves"

./build/bin/SaidaEngine.exe --project WitnessGame/WitnessGame.saidaproj \
    --build "$OUT" > "$ROOT/build/editor_build.log" 2>&1 || true

if ! grep -q "\[BUILD\] PASS" "$ROOT/build/editor_build.log"; then
    echo "EDITOR BUILD: FAIL (no [BUILD] PASS)"
    tail -20 "$ROOT/build/editor_build.log"
    exit 1
fi

# Runtime files (pipeline cache and logs) must never mutate the package that is
# handed to the archive/installer steps. Exercise a byte-for-byte copy instead.
cp -a "$OUT" "$RUN_OUT"
cd "$RUN_OUT"
export SAIDA_SAVE_DIR="$ROOT/build/witness-editor-build-saves"

"./Witness Game.exe" --test-autoload \
    "E2EDriver=scripts/e2e_driver.js" > "$ROOT/build/witness_editor_build_e2e.log" 2>&1 || true

if ! grep -q "\[E2E\] PASS" "$ROOT/build/witness_editor_build_e2e.log"; then
    echo "WITNESS EDITOR-BUILD E2E: FAIL"
    grep -E "\[E2E\]|\[JS\]|error" "$ROOT/build/witness_editor_build_e2e.log" | tail -20
    exit 1
fi

"./Witness Game.exe" --test-autoload \
    "E2EDriver=scripts/e2e_driver.js" > "$ROOT/build/witness_editor_build_restart.log" 2>&1 || true

if ! grep -q "\[E2E\] RESTART PASS" "$ROOT/build/witness_editor_build_restart.log"; then
    echo "WITNESS EDITOR-BUILD E2E: FAIL (restart)"
    grep -E "\[E2E\]|\[JS\]|error" "$ROOT/build/witness_editor_build_restart.log" | tail -20
    exit 1
fi

cd "$ROOT"
rm -rf "$RUN_OUT"
echo "WITNESS EDITOR-BUILD E2E: PASS (run + restart)"
exit 0
