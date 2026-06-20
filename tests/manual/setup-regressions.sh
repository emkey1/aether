#!/bin/sh
set -eu

src_dir=${ISH_AOK_TEST_SRC:-/AOK/tests}
work_dir=${ISH_AOK_REGRESS_DIR:-}
run_after=0
install_deps=0
run_args=
only_tests=

if [ -z "$work_dir" ]; then
    if [ -n "${TMPDIR:-}" ]; then
        work_dir=$TMPDIR/ish-aok-regressions
    elif [ -d /tmp ] && [ -w /tmp ]; then
        work_dir=/tmp/ish-aok-regressions
    else
        work_dir=./ish-aok-regressions
    fi
fi

usage() {
    cat <<EOF
Usage: $0 [--install-deps] [--run] [--src DIR] [--work DIR] [--only NAME[,NAME...]] [-v]

Stage, build, and optionally run the focused iSH-AOK guest regression suite.

Options:
  --install-deps  Install a minimal C toolchain if 'cc' is missing.
  --run           Run the compiled regression binaries after building them.
  -v, --verbose   Pass verbose output through to the regression binaries.
  --only LIST     Build only the comma-separated test names in LIST.
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
        --only)
            shift
            only_tests=$1
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

# amd64_regress.c uses x86-64-only inline asm (r8-r15, movq/popq) and exercises
# the amd64 JIT; it cannot assemble on an i686 guest. Require, build, and run it
# only on a 64-bit guest. Every other test is portable across i386 and amd64.
guest_arch=$(uname -m 2>/dev/null || echo unknown)
case "$guest_arch" in
    x86_64|amd64) is_amd64_guest=1 ;;
    *) is_amd64_guest=0 ;;
esac

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
need_file signal_poll.c
need_file eventfd_interrupt.c
need_file futex_core.c
need_file process_lifecycle.c
need_file pthread_sync.c
need_file ptrace_group_stop.c
need_file ptrace_thread_follow.c
need_file epoll_mod_wake.c
need_file epoll_oneshot_rearm.c
need_file ptrace_exit_kill.c
need_file fcntl_lock.c
need_file fcntl_ofd.c
need_file at_empty_path.c
need_file copy_file_range.c
need_file name_to_handle_at.c
need_file sendfile_vhangup.c
need_file pidfd_open.c
need_file fs_conformance.c
need_file process_conformance.c
need_file time_conformance.c
need_file mem_conformance.c
need_file sock_conformance.c
need_file netlink_route.c
need_file random_seed.c
if [ "$is_amd64_guest" -eq 1 ]; then
    need_file amd64_regress.c
fi

if ! mkdir -p "$work_dir/bin"; then
    echo "failed to create work directory: $work_dir" >&2
    exit 1
fi

gas_imm_reg_workaround=0
gas_probe=$work_dir/gas-imm-reg-probe.s
cat >"$gas_probe" <<'EOF'
.text
gas_imm_reg_probe:
    andl $1, %eax
    ret
EOF
if ! as -o "$work_dir/gas-imm-reg-probe.o" "$gas_probe" >/dev/null 2>&1; then
    gas_imm_reg_workaround=1
fi

write_gas_imm_reg_rewriter() {
    cat >"$work_dir/rewrite-gas-imm-reg.awk" <<'EOF'
function reg_number(reg, base) {
    sub(/^%/, "", reg)
    if (reg ~ /^r(1[0-5]|[8-9])[bwd]?$/) {
        sub(/^r/, "", reg)
        sub(/[bwd]$/, "", reg)
        return reg + 0
    }
    if (reg ~ /^r(1[0-5]|[8-9])$/) {
        sub(/^r/, "", reg)
        return reg + 0
    }
    if (reg == "al" || reg == "ax" || reg == "eax" || reg == "rax") return 0
    if (reg == "cl" || reg == "cx" || reg == "ecx" || reg == "rcx") return 1
    if (reg == "dl" || reg == "dx" || reg == "edx" || reg == "rdx") return 2
    if (reg == "bl" || reg == "bx" || reg == "ebx" || reg == "rbx") return 3
    if (reg == "ah" || reg == "sp" || reg == "esp" || reg == "rsp" || reg == "spl") return 4
    if (reg == "ch" || reg == "bp" || reg == "ebp" || reg == "rbp" || reg == "bpl") return 5
    if (reg == "dh" || reg == "si" || reg == "esi" || reg == "rsi" || reg == "sil") return 6
    if (reg == "bh" || reg == "di" || reg == "edi" || reg == "rdi" || reg == "dil") return 7
    return -1
}

function reg_needs_rex8(reg) {
    sub(/^%/, "", reg)
    return reg ~ /^(spl|bpl|sil|dil|r(1[0-5]|[8-9])b)$/
}

function byte_value(value) {
    value %= 256
    if (value < 0)
        value += 256
    return value
}

function emit_byte(value,    byte) {
    byte = byte_value(value)
    printf "%s0x%02x", sep, byte
    sep = ", "
}

function emit_imm(value, size,    limit, i) {
    limit = 1
    for (i = 0; i < size * 8; i++)
        limit *= 2
    if (value < 0)
        value += limit
    for (i = 0; i < size; i++) {
        emit_byte(value)
        value = int(value / 256)
    }
}

function group_for_op(op) {
    sub(/[bwlq]$/, "", op)
    if (op == "add") return 0
    if (op == "or") return 1
    if (op == "adc") return 2
    if (op == "sbb") return 3
    if (op == "and") return 4
    if (op == "sub") return 5
    if (op == "xor") return 6
    if (op == "cmp") return 7
    return -1
}

function rewrite_imm_reg(line,    tmp, parts, op, imm, reg, suffix, size, group, regnum, rex, opcode, imm_size, modrm) {
    tmp = line
    sub(/^[ \t]+/, "", tmp)
    split(tmp, parts, /[ \t,]+/)
    op = parts[1]
    imm = parts[2]
    reg = parts[3]
    if (op !~ /^(add|or|adc|sbb|and|sub|xor|cmp)[bwlq]$/ || imm !~ /^\$/ || reg !~ /^%/)
        return 0
    suffix = substr(op, length(op), 1)
    size = suffix == "b" ? 8 : (suffix == "w" ? 16 : (suffix == "l" ? 32 : 64))
    sub(/^\$/, "", imm)
    imm += 0
    group = group_for_op(op)
    regnum = reg_number(reg)
    if (group < 0 || regnum < 0)
        return 0

    rex = 0
    if (size == 64)
        rex += 8
    if (regnum >= 8)
        rex += 1
    if (size == 8 && reg_needs_rex8(reg))
        rex += 0

    if (size == 8) {
        opcode = 0x80
        imm_size = 1
    } else if (imm >= -128 && imm <= 127) {
        opcode = 0x83
        imm_size = 1
    } else {
        opcode = 0x81
        imm_size = size == 16 ? 2 : 4
    }
    modrm = 0xc0 + group * 8 + (regnum % 8)

    printf "\t.byte "
    sep = ""
    if (size == 16)
        emit_byte(0x66)
    if (rex != 0 || (size == 8 && reg_needs_rex8(reg)))
        emit_byte(0x40 + rex)
    emit_byte(opcode)
    emit_byte(modrm)
    emit_imm(imm, imm_size)
    printf "\n"
    return 1
}

function rewrite_mov_imm_reg(line,    tmp, parts, op, imm, reg, regnum, rex) {
    tmp = line
    sub(/^[ \t]+/, "", tmp)
    split(tmp, parts, /[ \t,]+/)
    op = parts[1]
    imm = parts[2]
    reg = parts[3]
    if (op != "movl" || imm !~ /^\$/ || reg !~ /^%/)
        return 0
    sub(/^\$/, "", imm)
    imm += 0
    regnum = reg_number(reg)
    if (regnum < 0)
        return 0

    rex = regnum >= 8 ? 1 : 0
    printf "\t.byte "
    sep = ""
    if (rex != 0)
        emit_byte(0x40 + rex)
    emit_byte(0xb8 + (regnum % 8))
    emit_imm(imm, 4)
    printf "\n"
    return 1
}

function rewrite_push_imm(line,    tmp, parts, op, imm) {
    tmp = line
    sub(/^[ \t]+/, "", tmp)
    split(tmp, parts, /[ \t,]+/)
    op = parts[1]
    imm = parts[2]
    if (op != "pushq" || imm !~ /^\$/)
        return 0
    sub(/^\$/, "", imm)
    imm += 0

    printf "\t.byte "
    sep = ""
    if (imm >= -128 && imm <= 127) {
        emit_byte(0x6a)
        emit_imm(imm, 1)
    } else {
        emit_byte(0x68)
        emit_imm(imm, 4)
    }
    printf "\n"
    return 1
}

{
    if (!rewrite_imm_reg($0) && !rewrite_mov_imm_reg($0) && !rewrite_push_imm($0))
        print
}
EOF
}

if [ "$gas_imm_reg_workaround" -eq 1 ]; then
    write_gas_imm_reg_rewriter
fi

build_one() {
    name=$1
    echo "+ build $name"
    if [ "$gas_imm_reg_workaround" -eq 0 ]; then
        cc -O2 -pthread -I"$src_dir" -o "$work_dir/bin/$name" "$src_dir/$name.c"
        return
    fi

    asm=$work_dir/$name.s
    fixed_asm=$work_dir/$name.gas-workaround.s
    cc -O2 -pthread -I"$src_dir" -S -o "$asm" "$src_dir/$name.c"
    awk -f "$work_dir/rewrite-gas-imm-reg.awk" "$asm" >"$fixed_asm"
    cc -pthread -o "$work_dir/bin/$name" "$fixed_asm"
}

all_tests="atomic_xadd32 atomic_cmpxchg32 atomic_cmpxchg8b atomic_logic32 signal_core signal_restart signal_realtime signal_altstack signal_poll eventfd_interrupt futex_core process_lifecycle pthread_sync ptrace_group_stop ptrace_thread_follow epoll_mod_wake epoll_oneshot_rearm ptrace_exit_kill fcntl_lock fcntl_ofd at_empty_path copy_file_range name_to_handle_at sendfile_vhangup pidfd_open fs_conformance process_conformance time_conformance mem_conformance sock_conformance netlink_route random_seed"
if [ "$is_amd64_guest" -eq 1 ]; then
    all_tests="$all_tests amd64_regress"
fi

test_selected() {
    name=$1
    if [ -z "$only_tests" ]; then
        return 0
    fi
    case ",$only_tests," in
        *,"$name",*) return 0 ;;
        *) return 1 ;;
    esac
}

selected_tests=
for test in $all_tests; do
    if test_selected "$test"; then
        build_one "$test"
        selected_tests="$selected_tests $test"
    fi
done

if [ -z "$selected_tests" ]; then
    echo "no regression tests selected" >&2
    exit 1
fi

cat >"$work_dir/run-regressions.sh" <<EOF
#!/bin/sh
set -eu

status=0
for test in$selected_tests; do
    echo "==> \$test"
    output="$work_dir/\$test.out"
    if "$work_dir/bin/\$test" "\$@" >"\$output" 2>&1; then
        rc=0
    else
        rc=\$?
    fi
    cat "\$output"
    if [ "\$rc" -ne 0 ]; then
        status=\$rc
        continue
    fi
    if ! grep -q "^\$test: PASS\$" "\$output"; then
        echo "\$test: FAIL missing PASS marker" >&2
        status=1
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
