#!/usr/bin/env bash
# Pre-compresses the web build artefacts with Brotli (Step 16.6). serve.py
# (and most static hosts / CDNs) will serve the .br files with
# Content-Encoding: br. Requires the `brotli` CLI.
set -e
DIR="${1:-build-web}"
command -v brotli >/dev/null 2>&1 || { echo "brotli CLI not found"; exit 1; }
for f in "$DIR"/index.wasm "$DIR"/index.js "$DIR"/index.data; do
    [ -f "$f" ] || continue
    brotli -f -q 11 "$f" -o "$f.br"
    printf "%-24s %10d -> %10d bytes\n" "$(basename "$f")" \
        "$(stat -c%s "$f")" "$(stat -c%s "$f.br")"
done
