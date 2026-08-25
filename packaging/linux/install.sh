#!/usr/bin/env bash
set -euo pipefail

version="1.0.0-beta.4"
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
payload="$script_dir/payload"

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
    echo "SaidaEngine requires Linux x86_64." >&2
    exit 1
fi
if [[ ! -x "$payload/SaidaEngineHub" || ! -f "$payload/PAYLOAD-SHA256SUMS.txt" ]]; then
    echo "Incomplete archive: payload or integrity manifest is missing." >&2
    exit 1
fi

echo "Verifying payload..."
(cd "$payload" && sha256sum --check --strict PAYLOAD-SHA256SUMS.txt)

data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
bin_home="${XDG_BIN_HOME:-$HOME/.local/bin}"
install_base="$data_home/SaidaEngine"
install_dir="$install_base/$version"
staging_dir="$install_base/.installing-$version-$$"
desktop_dir="$data_home/applications"

mkdir -p "$install_base" "$bin_home" "$desktop_dir"
if [[ -e "$install_dir" ]]; then
    if [[ ! -f "$install_dir/saida-install.json" ]]; then
        echo "Refusing to replace unrecognized directory: $install_dir" >&2
        exit 1
    fi
    rm -rf -- "$install_dir"
fi
rm -rf -- "$staging_dir"
mkdir -p "$staging_dir"
cp -a "$payload/." "$staging_dir/"
cp "$script_dir/uninstall.sh" "$staging_dir/uninstall.sh"
chmod +x "$staging_dir/uninstall.sh"
mv "$staging_dir" "$install_dir"

ln -sfn "$install_dir/SaidaEngineHub" "$bin_home/saidaengine-hub"
ln -sfn "$install_dir/SaidaEngine" "$bin_home/saidaengine-editor"
ln -sfn "$install_dir/saida_tool" "$bin_home/saidaengine-tool"

desktop_file="$desktop_dir/com.saidaengine.Editor.desktop"
{
    printf '%s\n' '[Desktop Entry]'
    printf '%s\n' 'Type=Application'
    printf '%s\n' 'Name=SaidaEngine'
    printf 'Comment=%s\n' 'SaidaEngine project Hub and editor'
    printf 'Exec=%s\n' "$bin_home/saidaengine-hub"
    printf 'Icon=%s\n' "$install_dir/assets/editor/saida_logo.png"
    printf '%s\n' 'Terminal=false'
    printf '%s\n' 'Categories=Development;IDE;Graphics;'
} > "$desktop_file"
chmod 0644 "$desktop_file"

echo "SaidaEngine $version installed for the current user."
echo "Launch: $bin_home/saidaengine-hub"
if [[ ":$PATH:" != *":$bin_home:"* ]]; then
    echo "Note: add $bin_home to PATH to use the short launcher names."
fi
