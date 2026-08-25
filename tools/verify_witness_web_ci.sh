#!/usr/bin/env bash
# Hermetic CI for the exported Web package: headers, boot of the real player,
# Witness scenario, IndexedDB save, then restoration after restart.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

STAGE="${1:-$ROOT/build/witness-web}"
BROWSER="${BROWSER_PATH:-$(command -v google-chrome || command -v chromium || true)}"
PORT="${SAIDA_WEB_E2E_PORT:-18080}"
TIMEOUT="${SAIDA_WEB_E2E_TIMEOUT:-90}"

if [[ ! -d "$STAGE" ]]; then
    echo "Witness Web stage not found: $STAGE" >&2
    exit 1
fi
if [[ -z "$BROWSER" || ! -x "$BROWSER" ]]; then
    echo "Chrome/Chromium not found; set BROWSER_PATH" >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/saida-web-e2e.XXXXXX")"
PROFILE="$WORK/browser-profile"
SERVER_LOG="$WORK/server.log"
BROWSER_LOG="$WORK/browser.log"
SERVER_PID=""
BROWSER_PID=""
BROWSER_OWNS_GROUP=false

stop_browser() {
    if [[ -n "$BROWSER_PID" ]] && kill -0 "$BROWSER_PID" 2>/dev/null; then
        if $BROWSER_OWNS_GROUP; then
            kill -- "-$BROWSER_PID" 2>/dev/null || true
        else
            kill "$BROWSER_PID" 2>/dev/null || true
        fi
        for _ in $(seq 1 20); do
            kill -0 "$BROWSER_PID" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$BROWSER_PID" 2>/dev/null; then
            if $BROWSER_OWNS_GROUP; then
                kill -KILL -- "-$BROWSER_PID" 2>/dev/null || true
            else
                kill -KILL "$BROWSER_PID" 2>/dev/null || true
            fi
        fi
    fi
    BROWSER_PID=""
    BROWSER_OWNS_GROUP=false
}

cleanup() {
    stop_browser
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

python3 web/serve.py "$STAGE" "$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
BASE_URL="http://127.0.0.1:$PORT"

for _ in $(seq 1 40); do
    if curl --fail --silent --output /dev/null "$BASE_URL/index.html"; then
        break
    fi
    sleep 0.25
done
curl --fail --silent --output /dev/null "$BASE_URL/index.html"

headers="$(curl --fail --silent --show-error --head "$BASE_URL/index.html" | tr -d '\r')"
grep -Eiq '^Cross-Origin-Opener-Policy: same-origin$' <<<"$headers"
grep -Eiq '^Cross-Origin-Embedder-Policy: require-corp$' <<<"$headers"
wasm_headers="$(curl --fail --silent --show-error --head "$BASE_URL/index.wasm" | tr -d '\r')"
grep -Eiq '^Content-Type: application/wasm$' <<<"$wasm_headers"

URL="$BASE_URL/?smoke&report&test-autoload=E2EDriver%3Dscripts%2Fe2e_driver.js"
CHROME_ARGS=(
    "--headless=new"
    "--no-sandbox"
    "--disable-dev-shm-usage"
    "--disable-background-networking"
    "--disable-component-update"
    "--disable-default-apps"
    "--disable-extensions"
    "--enable-logging=stderr"
    "--enable-unsafe-webgpu"
    "--enable-unsafe-swiftshader"
    # Force Chrome's bundled software renderer. `--use-angle=vulkan` selects
    # the runner's system Vulkan ICD first and fails on hosted machines without
    # VK_KHR_surface/VK_KHR_xcb_surface before Dawn can choose SwiftShader.
    "--use-angle=swiftshader"
    "--user-data-dir=$PROFILE"
    "--no-first-run"
    "--no-default-browser-check"
    "$URL"
)

start_browser() {
    if command -v setsid >/dev/null 2>&1; then
        setsid "$BROWSER" "${CHROME_ARGS[@]}" >>"$BROWSER_LOG" 2>&1 &
        BROWSER_OWNS_GROUP=true
    else
        "$BROWSER" "${CHROME_ARGS[@]}" >>"$BROWSER_LOG" 2>&1 &
        BROWSER_OWNS_GROUP=false
    fi
    BROWSER_PID=$!
}

wait_verdict() {
    local expected="$1"
    local deadline=$((SECONDS + TIMEOUT))
    local report=""
    while (( SECONDS < deadline )); do
        report="$(curl --fail --silent "$BASE_URL/__saida_e2e" 2>/dev/null || true)"
        if grep -Fq "[E2E] FAIL" <<<"$report"; then
            echo "Witness Web reported failure: $report" >&2
            return 1
        fi
        if grep -Fq "$expected" <<<"$report"; then
            printf '%s\n' "$report"
            return 0
        fi
        if [[ -n "$BROWSER_PID" ]] && ! kill -0 "$BROWSER_PID" 2>/dev/null; then
            echo "Browser exited before verdict $expected" >&2
            return 1
        fi
        sleep 0.25
    done
    echo "Timed out waiting for verdict $expected; last report: $report" >&2
    return 1
}

start_browser
if ! first="$(wait_verdict "[E2E] PASS")"; then
    tail -100 "$BROWSER_LOG" >&2 || true
    exit 1
fi

stop_browser
curl --fail --silent --request POST --output /dev/null "$BASE_URL/__saida_e2e/reset"
start_browser
if ! restart="$(wait_verdict "[E2E] RESTART PASS")"; then
    tail -100 "$BROWSER_LOG" >&2 || true
    exit 1
fi

echo "WITNESS WEB CI: PASS"
echo "  browser: $BROWSER"
echo "  first run: $first"
echo "  restart: $restart"
