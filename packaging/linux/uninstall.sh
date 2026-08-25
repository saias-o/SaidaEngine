#!/usr/bin/env bash
set -euo pipefail

version="1.0.0-beta.4"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
bin_home="${XDG_BIN_HOME:-$HOME/.local/bin}"
install_dir="$data_home/SaidaEngine/$version"

if [[ ! -f "$install_dir/saida-install.json" ]]; then
    echo "SaidaEngine $version is not installed at $install_dir." >&2
    exit 1
fi

for launcher in saidaengine-hub saidaengine-editor saidaengine-tool; do
    link="$bin_home/$launcher"
    if [[ -L "$link" && "$(readlink -f -- "$link")" == "$install_dir/"* ]]; then
        rm -- "$link"
    fi
done

desktop_file="$data_home/applications/com.saidaengine.Editor.desktop"
if [[ -f "$desktop_file" ]] && grep -Fq "$install_dir" "$desktop_file"; then
    rm -- "$desktop_file"
fi

rm -rf -- "$install_dir"
echo "SaidaEngine $version uninstalled. Projects and user state were kept."
