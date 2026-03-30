#!/bin/bash

# Try to figure out the user's PATH to pick up their installed utilities.
export PATH="$PATH:$(sudo -u "$USER" -i printenv PATH)"

set -euo pipefail

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
