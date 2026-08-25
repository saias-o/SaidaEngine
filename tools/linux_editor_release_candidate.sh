#!/usr/bin/env bash
set -euo pipefail

version="${1:-1.0.0-beta.4}"
output="${2:-build/release/linux-editor}"
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
    echo "This candidate recipe requires Linux x86_64." >&2
    exit 1
fi
if [[ -n "$(git status --short)" ]]; then
    echo "The release worktree must be clean." >&2
    exit 1
fi

release_base="$(realpath -m -- "$root/build/release")"
output="$(realpath -m -- "$output")"
case "$output" in
    "$release_base"/*) ;;
    *) echo "Output must be a child of $release_base" >&2; exit 1 ;;
esac

commit="$(git rev-parse HEAD)"
commit_time="$(git show -s --format=%ct HEAD)"
product="SaidaEngine-v${version}-linux-x86_64"
archive="$output/$product.tar.gz"
stage="$output/$product"
payload="$stage/payload"

glslc_option=()
if ! command -v glslc >/dev/null 2>&1; then
    command -v glslangValidator >/dev/null 2>&1 || {
        echo "Neither glslc nor glslangValidator is installed." >&2
        exit 1
    }
    glslc_option=("-DGLSLC=$root/tools/glslc_from_glslang.sh")
fi

rm -rf -- "$output"
mkdir -p "$payload/lib" "$payload/assets/fonts" "$payload/samples"

cmake -S . -B build/linux-release -G Ninja \
    "${glslc_option[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSAIDA_ENABLE_XR=OFF \
    -DSAIDA_ENABLE_MCP=OFF \
    -DUSE_SSE4_1=OFF -DUSE_SSE4_2=OFF -DUSE_AVX=OFF -DUSE_AVX2=OFF \
    -DUSE_LZCNT=OFF -DUSE_TZCNT=OFF -DUSE_F16C=OFF -DUSE_FMADD=OFF \
    -DCMAKE_EXE_LINKER_FLAGS='-Wl,--enable-new-dtags,-rpath,$ORIGIN/lib'
cmake --build build/linux-release --target \
    SaidaEngine SaidaEngineHub SaidaEngineRuntime saida_tool
ctest --test-dir build/linux-release --output-on-failure

for name in SaidaEngine SaidaEngineHub SaidaEngineRuntime saida_tool; do
    install -m 0755 "build/linux-release/bin/$name" "$payload/$name"
done
cp -a assets/. "$payload/assets/"
for font in LatoLatin-Regular.ttf LatoLatin-Bold.ttf RobotoMono-Regular.ttf NotoEmoji-Regular.ttf; do
    install -m 0644 "third_party/rmlui/Samples/assets/$font" "$payload/assets/fonts/$font"
done
cp -a build/linux-release/shaders "$payload/shaders"
cp -a WitnessGame "$payload/samples/WitnessGame"
rm -rf -- "$payload/samples/WitnessGame/saves"
install -m 0644 LICENSE RELEASE.md "$payload/"

declare -A queued=()
declare -a queue=()
for name in SaidaEngine SaidaEngineHub SaidaEngineRuntime saida_tool; do
    queue+=("$payload/$name")
done

is_glibc_runtime() {
    case "$1" in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libutil.so.*|libresolv.so.*|ld-linux-x86-64.so.*)
            return 0 ;;
        *) return 1 ;;
    esac
}

index=0
while (( index < ${#queue[@]} )); do
    elf="${queue[$index]}"
    index=$((index + 1))
    while IFS= read -r dependency; do
        [[ -n "$dependency" && -f "$dependency" ]] || continue
        soname="$(basename -- "$dependency")"
        is_glibc_runtime "$soname" && continue
        [[ -n "${queued[$soname]:-}" ]] && continue
        queued[$soname]=1
        install -m 0755 "$dependency" "$payload/lib/$soname"
        queue+=("$payload/lib/$soname")
    done < <(ldd "$elf" | awk '/=> \/[^ ]+/ {print $3} /^\/[^ ]+/ {print $1}')
done

for elf in "$payload/SaidaEngine" "$payload/SaidaEngineHub" \
           "$payload/SaidaEngineRuntime" "$payload/saida_tool"; do
    patchelf --set-rpath '$ORIGIN/lib' "$elf"
done
for elf in "$payload"/lib/*; do
    patchelf --set-rpath '$ORIGIN' "$elf"
done

for name in SaidaEngine SaidaEngineHub SaidaEngineRuntime saida_tool; do
    if LD_LIBRARY_PATH="$payload/lib" ldd "$payload/$name" | grep -q 'not found'; then
        LD_LIBRARY_PATH="$payload/lib" ldd "$payload/$name" >&2
        exit 1
    fi
done

{
    echo "SaidaEngine Linux dependency closure"
    echo "commit=$commit"
    echo "glibc-baseline=2.35"
    echo
    for name in SaidaEngine SaidaEngineHub SaidaEngineRuntime saida_tool; do
        echo "[$name]"
        LD_LIBRARY_PATH="$payload/lib" ldd "$payload/$name"
        echo
    done
} > "$payload/linux-dependencies.txt"

printf '{\n  "schema": 1,\n  "product": "SaidaEngine",\n  "version": "%s",\n  "platform": "linux-x86_64",\n  "commit": "%s",\n  "entryPoint": "SaidaEngineHub",\n  "additionalRuntimeInstall": false\n}\n' \
    "$version" "$commit" > "$payload/saida-install.json"

install -m 0644 packaging/linux/README-LINUX.md "$stage/README-LINUX.md"
install -m 0755 packaging/linux/install.sh packaging/linux/uninstall.sh "$stage/"

(cd "$payload" && find . -type f ! -name PAYLOAD-SHA256SUMS.txt -print0 | \
    sort -z | xargs -0 sha256sum > PAYLOAD-SHA256SUMS.txt)

find "$stage" -exec touch --no-dereference --date="@$commit_time" {} +
tar --sort=name --format=posix --mtime="@$commit_time" --owner=0 --group=0 \
    --numeric-owner -C "$output" -cf - "$product" | gzip -n > "$archive"

archive_hash="$(sha256sum "$archive" | awk '{print $1}')"
printf '%s  %s\n' "$archive_hash" "$(basename "$archive")" \
    > "$output/SHA256SUMS-LINUX.txt"
cp packaging/linux/README-LINUX.md "$output/README-LINUX.md"
printf '{\n  "schema": 1,\n  "version": "%s",\n  "commit": "%s",\n  "archive": "%s",\n  "sha256": "%s",\n  "dirty": false,\n  "qualified": false\n}\n' \
    "$version" "$commit" "$(basename "$archive")" "$archive_hash" \
    > "$output/release-manifest-linux.json"

rm -rf -- "$stage"
echo "Linux editor candidate ready: $archive"
