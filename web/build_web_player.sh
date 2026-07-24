#!/usr/bin/env bash
set -e

CONFIG="${1:-Release}"

if ! command -v emcmake >/dev/null 2>&1; then
    for candidate in "$HOME/emsdk/emsdk_env.sh" "/c/Users/evand/emsdk/emsdk_env.sh"; do
        if [ -f "$candidate" ]; then
            source "$candidate" >/dev/null 2>&1
            break
        fi
    done
fi

command -v emcmake >/dev/null 2>&1 || { echo "emsdk not found"; exit 1; }

if [ ! -f build/shaders/wgsl/shader.frag.wgsl ]; then
    echo "WGSL shaders missing — run: cmake --build build --target WebShaders"
    exit 1
fi

emcmake cmake -S web/player -B build-web-player -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build build-web-player
rm -f build-web-player/*.br

echo "web player template: build-web-player/index.html"
