#!/usr/bin/env bash
set -euo pipefail

archive="${1:?usage: verify_linux_editor_archive.sh <archive> [manifest]}"
manifest="${2:-$(dirname -- "$archive")/release-manifest-linux.json}"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

expected="$(sed -n 's/.*"sha256": "\([0-9a-f]\{64\}\)".*/\1/p' "$manifest")"
actual="$(sha256sum "$archive" | awk '{print $1}')"
[[ -n "$expected" && "$actual" == "$expected" ]]

tar -xzf "$archive" -C "$work"
package_dir="$(find "$work" -mindepth 1 -maxdepth 1 -type d -name 'SaidaEngine-v*-linux-x86_64' -print -quit)"
[[ -n "$package_dir" ]]

export HOME="$work/home"
export XDG_DATA_HOME="$HOME/.local/share"
export XDG_BIN_HOME="$HOME/.local/bin"
mkdir -p "$HOME"
"$package_dir/install.sh"

install_dir="$(find "$XDG_DATA_HOME/SaidaEngine" -mindepth 1 -maxdepth 1 -type d -print -quit)"
[[ -x "$install_dir/SaidaEngineHub" && -x "$install_dir/SaidaEngineRuntime" ]]
"$XDG_BIN_HOME/saidaengine-tool" describe-engine > "$work/engine-manifest.json"
grep -q '"nodes"' "$work/engine-manifest.json"

export LIBGL_ALWAYS_SOFTWARE=1
export VK_LOADER_DRIVERS_SELECT='lvp_icd.x86_64.json'
xvfb-run -a "$install_dir/SaidaEngineHub" --verify-installation
xvfb-run -a "$install_dir/SaidaEngine" --project \
    "$install_dir/samples/WitnessGame/WitnessGame.saidaproj" \
    --build "$work/export" --build-platform native

game="$(find "$work/export" -maxdepth 1 -type f -perm /111 -print -quit)"
[[ -n "$game" && -d "$work/export/lib" && -d "$work/export/shaders" ]]
if LD_LIBRARY_PATH="$work/export/lib" ldd "$game" | grep -q 'not found'; then
    LD_LIBRARY_PATH="$work/export/lib" ldd "$game" >&2
    exit 1
fi

"$install_dir/uninstall.sh"
[[ ! -e "$install_dir" ]]
sed -i 's/"qualified": false/"qualified": true/' "$manifest"
grep -q '"qualified": true' "$manifest"
printf '{\n  "qualified": true,\n  "archiveSha256": "%s",\n  "checks": [\n    "payload hashes",\n    "per-user install",\n    "headless CLI",\n    "Hub Vulkan startup under Xvfb/lavapipe",\n    "installed editor native export",\n    "exported ELF dependency closure",\n    "uninstall"\n  ]\n}\n' "$actual" > "$(dirname -- "$archive")/linux-qualification.json"
echo "VERIFY LINUX EDITOR PASS"
