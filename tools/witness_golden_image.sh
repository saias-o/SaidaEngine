#!/usr/bin/env bash
# Golden-image gate: render a fixed scene and prove it still looks the same.
#
# This is the §3 visual net. It exports WitnessGame, captures one frame of the
# hub scene with the player (asset wait + pinned clock, so the frame is the same
# moment every run), and compares it pixel-for-pixel against a committed
# reference.
#
#   tools/witness_golden_image.sh            gate: exit 1 if the frame changed
#   tools/witness_golden_image.sh --record   rewrite the reference deliberately
#
# The reference is keyed to LAVAPIPE, and the script refuses to run on anything
# else. Two rasterizers do not agree on a lit surface: the same build on this
# repo's CI (llvmpipe) and on an Intel Iris Xe differs by 69 268 of 230 400
# pixels — a banding pattern across every lit surface, worst channel delta 90.
# No tolerance separates that from a real regression, so the gate uses exact
# equality against a software rasterizer that IS deterministic, and a real GPU
# is simply not gated. Refusing beats gating the wrong backend: a reference
# recorded on a GPU would make every CI run fail for a reason that is not a bug.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

RECORD=0
[ "${1:-}" = "--record" ] && RECORD=1

REFERENCE="$ROOT/tests/fixtures/golden/witness-hub.lavapipe.png"
OUT="$ROOT/build/golden-image-run"
ACTUAL="$OUT/frame.png"
DIFF="$ROOT/build/golden-image-diff.png"
LOG="$ROOT/build/golden_image.log"

# Frame 30 of the settled sequence, on a 1/60 clock: half a simulated second,
# far enough in that animation and physics have reached a defined state rather
# than their first tick.
FRAME=30

TOOL="$ROOT/build/bin/saida_tool.exe"
[ -x "$TOOL" ] || TOOL="$ROOT/build/bin/saida_tool"

rm -rf "$OUT"
rm -f "$DIFF"

# --out is resolved relative to the PROJECT directory, not the caller's cwd, so
# it is passed absolute here.
"$TOOL" export-game WitnessGame/WitnessGame.saidaproj --platform windows \
    --out "$OUT" > "$LOG" 2>&1

cd "$OUT"
SAIDA_WINDOW_HIDDEN=1 "./Witness Game.exe" --screenshot "$ACTUAL" \
    --after-frames "$FRAME" >> "$LOG" 2>&1
cd "$ROOT"

if ! grep -q "\[capture\] wrote" "$LOG"; then
    echo "GOLDEN IMAGE: FAIL (no frame captured)"
    grep -iE "\[capture\]|error" "$LOG" | tail -20
    exit 1
fi

# The renderer names its device on startup. Gating a capture from another
# backend against this reference would report a difference that is not a defect.
if ! grep -q "GPU: llvmpipe" "$LOG"; then
    echo "GOLDEN IMAGE: SKIP (not Lavapipe)"
    grep "GPU:" "$LOG" | tail -1
    echo "  The reference is a software-rasterizer image. Register the Lavapipe"
    echo "  ICD before running this gate:"
    echo "    export VK_DRIVER_FILES=\"\$(cygpath -w /ucrt64/share/vulkan/icd.d/lvp_icd.x86_64.json)\""
    echo "    export VK_ICD_FILENAMES=\"\$VK_DRIVER_FILES\""
    exit 1
fi

if [ "$RECORD" = "1" ]; then
    mkdir -p "$(dirname "$REFERENCE")"
    cp "$ACTUAL" "$REFERENCE"
    echo "GOLDEN IMAGE: RECORDED $(basename "$REFERENCE")"
    echo "  Look at the image before committing it. This gate proves the frame"
    echo "  does not CHANGE; only a human can say it is RIGHT."
    exit 0
fi

if [ ! -f "$REFERENCE" ]; then
    echo "GOLDEN IMAGE: FAIL (no reference at ${REFERENCE#$ROOT/})"
    echo "  Record one with: tools/witness_golden_image.sh --record"
    exit 1
fi

if "$TOOL" compare-png "$ACTUAL" "$REFERENCE" --diff "$DIFF"; then
    # The export carries a full runtime executable; only a failure needs it kept
    # (the captured frame is half of what makes a red run diagnosable).
    rm -rf "$OUT"
    rm -f "$DIFF"
    echo "GOLDEN IMAGE: PASS"
    exit 0
fi

echo "GOLDEN IMAGE: FAIL (the hub scene no longer renders identically)"
echo "  reference: ${REFERENCE#$ROOT/}"
echo "  captured:  ${ACTUAL#$ROOT/}"
echo "  diff:      ${DIFF#$ROOT/} (reference in grey, changed pixels in magenta)"
echo "  If the change is intended, look at the capture, then re-record with"
echo "  tools/witness_golden_image.sh --record"
exit 1
