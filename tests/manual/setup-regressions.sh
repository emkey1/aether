#!/bin/sh
set -eu

src_dir=${ISH_AOK_TEST_SRC:-/AOK/tests}
work_dir=${ISH_AOK_REGRESS_DIR:-${TMPDIR:-/tmp}/ish-aok-regressions}
run_after=0
install_deps=0
run_args=

usage() {
    cat <<EOF
Usage: $0 [--install-deps] [--run] [--src DIR] [--work DIR] [-v]

Stage, build, and optionally run the focused iSH-AOK guest regression suite.

Options:
  --install-deps  Install a minimal C toolchain if 'cc' is missing.
  --run           Run the compiled regression binaries after building them.
  -v, --verbose   Pass verbose output through to the regression binaries.
  --src DIR       Source directory containing the regression test sources.
                  Default: /AOK/tests
  --work DIR      Output directory for binaries and the generated runner.
                  Default: /tmp/ish-aok-regressions
  -h, --help      Show this help text.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --install-deps)
            install_deps=1
            ;;
        --run)
            run_after=1
            ;;
        -v|--verbose)
            run_args="$run_args --verbose"
            ;;
        --src)
            shift
            src_dir=$1
            ;;
        --work)
            shift
            work_dir=$1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

need_file() {
    if [ ! -r "$src_dir/$1" ]; then
        echo "missing required test source: $src_dir/$1" >&2
        exit 1
    fi
}

install_toolchain() {
    if command -v apk >/dev/null 2>&1; then
        apk add --no-cache build-base
        return
    fi
    if command -v apt-get >/dev/null 2>&1; then
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential
        return
    fi
    echo "unsupported package manager; install a working C toolchain manually" >&2
    exit 1
}

if ! command -v cc >/dev/null 2>&1; then
    if [ "$install_deps" -eq 1 ]; then
        install_toolchain
    else
        echo "'cc' not found; rerun with --install-deps or install a C toolchain first" >&2
        exit 1
    fi
fi

need_file atomic_common.h
need_file test_common.h
need_file atomic_xadd32.c
need_file atomic_cmpxchg32.c
need_file atomic_cmpxchg8b.c
need_file atomic_logic32.c
need_file signal_core.c
need_file signal_restart.c
need_file signal_realtime.c
need_file signal_altstack.c
need_file futex_core.c

mkdir -p "$work_dir/bin"

build_one() {
    name=$1
    cc -O2 -pthread -I"$src_dir" -o "$work_dir/bin/$name" "$src_dir/$name.c"
}

build_one atomic_xadd32
build_one atomic_cmpxchg32
build_one atomic_cmpxchg8b
build_one atomic_logic32
build_one signal_core
build_one signal_restart
build_one signal_realtime
build_one signal_altstack
build_one futex_core

cat >"$work_dir/run-regressions.sh" <<EOF
#!/bin/sh
set -eu

status=0
for test in atomic_xadd32 atomic_cmpxchg32 atomic_cmpxchg8b atomic_logic32 signal_core signal_restart signal_realtime signal_altstack futex_core; do
    echo "==> \$test"
    if ! "$work_dir/bin/\$test" "\$@"; then
        status=\$?
    fi
done
exit \$status
EOF
chmod +x "$work_dir/run-regressions.sh"

echo "Regression binaries are in $work_dir/bin"
echo "Runner is $work_dir/run-regressions.sh"

if [ "$run_after" -eq 1 ]; then
    exec "$work_dir/run-regressions.sh" $run_args
fi
