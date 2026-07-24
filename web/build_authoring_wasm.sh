#!/usr/bin/env bash
# Builds the headless authoring-core validation WASM. Loaded in-process by the
# Node Collaboration Gateway
# to validate SaidaOps with the real C++ contract (zero duplication, no latency).
#
#   ./web/build_authoring_wasm.sh [Release|Debug]
#
# Requires: emsdk (activated automatically below if found).
# Output: build-authoring-wasm/saida_authoring.mjs + saida_authoring.wasm

set -e
CONFIG="${1:-Release}"

if ! command -v emcmake >/dev/null 2>&1; then
    for candidate in "$HOME/emsdk/emsdk_env.sh" "/c/Users/evand/emsdk/emsdk_env.sh"; do
        if [ -f "$candidate" ]; then
            # shellcheck disable=SC1090
            source "$candidate" >/dev/null 2>&1
            break
        fi
    done
fi
command -v emcmake >/dev/null 2>&1 || { echo "emsdk not found — activate it first"; exit 1; }

emcmake cmake -S web/authoring -B build-authoring-wasm -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build build-authoring-wasm

echo
echo "sizes:"
ls -la build-authoring-wasm/saida_authoring.* | awk '{printf "  %-26s %10d bytes\n", $NF, $5}'
