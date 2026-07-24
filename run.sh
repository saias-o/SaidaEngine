#!/usr/bin/env bash
# Launches SaidaEngine with Vulkan validation layers enabled.
#
# The layers (MSYS2 package mingw-w64-ucrt-x86_64-vulkan-validation-layers) are
# not registered with the loader: point VK_LAYER_PATH at the manifest's
# folder, and make sure ucrt64/bin is at the front of PATH so the DLLs the
# layer depends on (libstdc++, SPIRV-Tools) resolve correctly.
set -euo pipefail

# ucrt64/bin = gcc's folder (hence vulkan-1.dll and the layer manifest too).
ucrt_bin="$(dirname "$(command -v gcc)")"
export PATH="$ucrt_bin:$PATH"
export VK_LAYER_PATH="$(cygpath -w "$ucrt_bin")"

cd "$(dirname "$0")/build/bin"
exec ./SaidaEngine.exe "$@"
