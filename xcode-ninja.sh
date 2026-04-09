#!/bin/bash

set -euo pipefail

bootstrap_path() {
    local extra
    for extra in /opt/homebrew/bin /opt/homebrew/sbin /usr/local/bin /usr/local/sbin; do
        case ":$PATH:" in
            *":$extra:"*) ;;
            *) PATH="$PATH:$extra" ;;
        esac
    done
    export PATH
}

bootstrap_path

if ! command -v ninja >/dev/null 2>&1; then
    echo "ninja not found in PATH: $PATH" >&2
    exit 1
fi

declare -a arch_list=()
if [[ -n "${ARCHS:-}" ]]; then
    for arch in $ARCHS; do
        arch_list+=("$arch")
    done
else
    arch_list+=("${CURRENT_ARCH:-$(uname -m)}")
fi

for arch in "${arch_list[@]}"; do
    ninja -C "$MESON_BUILD_DIR/$arch" "$@"
done

for target in "$@"; do
    declare -a inputs=()
    output="$MESON_BUILD_DIR/$target"
    mkdir -p "$(dirname "$output")"
    for arch in "${arch_list[@]}"; do
        input="$MESON_BUILD_DIR/$arch/$target"
        if [[ ! -f "$input" ]]; then
            echo "Missing Meson output for $arch: $input" >&2
            exit 1
        fi
        inputs+=("$input")
    done

    rm -f "$output"
    if (( ${#inputs[@]} == 1 )); then
        ln -sf "${inputs[0]}" "$output"
    else
        lipo -create -output "$output" "${inputs[@]}"
    fi
done
