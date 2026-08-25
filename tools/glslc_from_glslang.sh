#!/usr/bin/env bash
set -euo pipefail

# Minimal glslc-compatible adapter for distributions that package
# glslangValidator but not the shaderc command-line compiler. SaidaEngine's
# shader recipe uses only -I, -D, input and -o, all supported here.
args=(-V)
while (($#)); do
    if [[ "$1" == "-I" ]]; then
        [[ $# -ge 2 ]] || { echo "-I requires a directory" >&2; exit 2; }
        args+=("-I$2")
        shift 2
    else
        args+=("$1")
        shift
    fi
done
exec glslangValidator "${args[@]}"
