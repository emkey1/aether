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

if ! command -v meson >/dev/null 2>&1; then
    echo "meson not found in PATH: $PATH" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found in PATH: $PATH" >&2
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

mkdir -p "$MESON_BUILD_DIR"

meson_cpu_family() {
    local arch="$1"
    case "$arch" in
        arm64) echo aarch64 ;;
        *) echo "$arch" ;;
    esac
}

configure_arch() {
    local arch="$1"
    local meson_dir="$MESON_BUILD_DIR/$arch"
    local crossfile="$meson_dir/cross.txt"
    local meson_arch
    local config

    mkdir -p "$meson_dir"
    cd "$meson_dir"

    if [[ ! -f meson-private/coredata.dat ]]; then
        export CC_FOR_BUILD="env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun clang"
        export CC="$CC_FOR_BUILD" # compatibility with meson < 0.54.0
        meson_arch=$(meson_cpu_family "$arch")
        cat >"$crossfile" <<-EOF
	[binaries]
	c = 'clang'
	ar = 'ar'

	[host_machine]
	system = 'darwin'
	cpu_family = '$meson_arch'
	cpu = '$meson_arch'
	endian = 'little'

	[built-in options]
	c_args = ['-arch', '$arch']

	[properties]
	needs_exe_wrapper = true
EOF
        (set -x; meson "$SRCROOT" --cross-file "$crossfile") || exit $?
    fi
    config=$(meson introspect --buildoptions)

    buildtype=debug
    b_ndebug=false
    if [[ $CONFIGURATION == Release ]]; then
        buildtype=debugoptimized
    fi
    b_sanitize=none
    if [[ -n "${ENABLE_ADDRESS_SANITIZER:-}" ]]; then
        b_sanitize=address
    fi
    log=${ISH_LOG:-}
    log_handler=${ISH_LOGGER:-}
    kernel=ish
    if [[ -n "${ISH_KERNEL:-}" ]]; then
        kernel=$ISH_KERNEL
    fi
    kconfig=""
    for var in buildtype log b_ndebug b_sanitize log_handler kernel kconfig; do
        old_value=$(python3 -c "import sys, json; v = next(x['value'] for x in json.load(sys.stdin) if x['name'] == '$var'); print(str(v).lower() if isinstance(v, bool) else ','.join(v) if isinstance(v, list) else v)" <<< "$config")
        new_value=${!var}
        if [[ $old_value != $new_value ]]; then
            set -x; meson configure "-D$var=$new_value"
        fi
    done
}

for arch in "${arch_list[@]}"; do
    configure_arch "$arch"
done
