#!/usr/bin/env bash
# Stages the exact web build of the witness game via the REAL BuildExporter
# (saida_tool export-game --platform web, the same code path as the Build
# button). The E2E driver is passed to the runtime through the URL and never
# modifies any file in the package.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

EXPORT="$ROOT/build/witness-web-export"
OUT="${1:-$ROOT/build/witness-web}"
rm -rf "$EXPORT" "$OUT"

"${SAIDA_TOOL:-$(
    if [[ -x "$ROOT/build/bin/saida_tool.exe" ]]; then
        printf '%s' "$ROOT/build/bin/saida_tool.exe"
    else
        printf '%s' "$ROOT/build/bin/saida_tool"
    fi
)}" export-game WitnessGame/WitnessGame.saidaproj \
    --platform web --out "$EXPORT" > /dev/null

# exportWebBuild writes <out>/web; publish that folder as-is.
mv "$EXPORT/web" "$OUT"
rm -rf "$EXPORT"

echo "staged: $OUT"
echo "serve: python web/serve.py $OUT 8080"
echo "test:  http://localhost:8080/?smoke&test-autoload=E2EDriver%3Dscripts%2Fe2e_driver.js"
