#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

ISH_BIN=./build/ish
ROOTFS=.tmp-root-amd64-gcc/data
OUT_DIR=
MODES=interp,jit
GUEST_SHELL=

usage() {
    cat <<'EOF'
Usage: tests/manual/amd64_gas_probe.sh [options]

Run a focused amd64 GNU as encoding probe inside an amd64 guest rootfs.

The probe assembles one source file containing both suspect mnemonics and
paired expected encodings emitted as .byte directives. It then compares the
instruction bytes produced for each mnemonic against the paired .byte label.

Options:
  -b BIN      Path to iSH host binary. Default: ./build/ish
  -r ROOTFS   Path to amd64 guest rootfs. Default: .tmp-root-amd64-gcc/data
  -o DIR      Output directory. Default: ./e2e_out/amd64-gas-probe-<timestamp>
  -m LIST     Comma-separated modes: interp,jit,both. Default: interp,jit
  -s COMMAND  Guest shell command. Default: /bin/sh, or /bin/busybox sh when
              /bin/sh is not executable in the selected rootfs.
  -h          Show this help

The rootfs must contain /usr/bin/as and /usr/bin/objdump.
EOF
}

while getopts ":b:r:o:m:s:h" opt; do
    case "$opt" in
        b) ISH_BIN=$OPTARG ;;
        r) ROOTFS=$OPTARG ;;
        o) OUT_DIR=$OPTARG ;;
        m) MODES=$OPTARG ;;
        s) GUEST_SHELL=$OPTARG ;;
        h)
            usage
            exit 0
            ;;
        :)
            echo "missing argument for -$OPTARG" >&2
            usage >&2
            exit 2
            ;;
        \?)
            echo "unknown option: -$OPTARG" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case ",$MODES," in
    *,both,*) MODES=interp,jit ;;
esac

if [[ ! -x $ISH_BIN ]]; then
    echo "iSH binary is not executable: $ISH_BIN" >&2
    exit 1
fi
if [[ ! -d $ROOTFS ]]; then
    echo "guest rootfs not found: $ROOTFS" >&2
    exit 1
fi
if [[ ! -e $ROOTFS/usr/bin/as || ! -e $ROOTFS/usr/bin/objdump ]]; then
    echo "guest rootfs must contain /usr/bin/as and /usr/bin/objdump: $ROOTFS" >&2
    exit 1
fi
if [[ -z ${GUEST_SHELL:-} ]]; then
    if [[ -x $ROOTFS/bin/sh ]]; then
        GUEST_SHELL=/bin/sh
    elif [[ -x $ROOTFS/bin/busybox ]]; then
        GUEST_SHELL="/bin/busybox sh"
    else
        GUEST_SHELL=/bin/sh
    fi
fi
read -r -a GUEST_SHELL_ARGV <<<"$GUEST_SHELL"

if [[ -z ${OUT_DIR:-} ]]; then
    OUT_DIR=./e2e_out/amd64-gas-probe-$(date +%Y%m%d-%H%M%S)
fi
mkdir -p "$OUT_DIR"

GUEST_WORK=/tmp/ish-aok-amd64-gas-probe
HOST_WORK=$ROOTFS$GUEST_WORK
rm -rf "$HOST_WORK"
mkdir -p "$HOST_WORK"

cat >"$HOST_WORK/probe.s" <<'EOF'
.text
.globl _start
_start:
    nop

.macro probe name:req, expected:vararg
case_\name:
    \name
    ret
expect_\name:
    .byte \expected
    ret
.endm

/* Immediate/register encodings that previously needed GAS workarounds. */
case_andl_imm_eax:
    andl $1, %eax
    ret
expect_andl_imm_eax:
    .byte 0x83, 0xe0, 0x01
    ret

case_orl_imm_r8d:
    orl $127, %r8d
    ret
expect_orl_imm_r8d:
    .byte 0x41, 0x83, 0xc8, 0x7f
    ret

case_xorl_imm_r9d:
    xorl $-1, %r9d
    ret
expect_xorl_imm_r9d:
    .byte 0x41, 0x83, 0xf1, 0xff
    ret

case_subl_imm_r10d:
    subl $0x12345678, %r10d
    ret
expect_subl_imm_r10d:
    .byte 0x41, 0x81, 0xea, 0x78, 0x56, 0x34, 0x12
    ret

case_cmpq_imm_r11:
    cmpq $-1, %r11
    ret
expect_cmpq_imm_r11:
    .byte 0x49, 0x83, 0xfb, 0xff
    ret

case_addb_imm_spl:
    addb $1, %spl
    ret
expect_addb_imm_spl:
    .byte 0x40, 0x80, 0xc4, 0x01
    ret

case_movl_imm_r8d:
    movl $0x12345678, %r8d
    ret
expect_movl_imm_r8d:
    .byte 0x41, 0xb8, 0x78, 0x56, 0x34, 0x12
    ret

case_pushq_imm8:
    pushq $1
    ret
expect_pushq_imm8:
    .byte 0x6a, 0x01
    ret

case_pushq_imm32:
    pushq $0x12345678
    ret
expect_pushq_imm32:
    .byte 0x68, 0x78, 0x56, 0x34, 0x12
    ret
EOF

cat >"$HOST_WORK/expected-labels.txt" <<'EOF'
andl_imm_eax
orl_imm_r8d
xorl_imm_r9d
subl_imm_r10d
cmpq_imm_r11
addb_imm_spl
movl_imm_r8d
pushq_imm8
pushq_imm32
EOF

split_cases() {
    local name

    while IFS= read -r name; do
        {
            printf '%s\n' '.text'
            printf '%s\n' '.globl _start'
            printf '%s\n' '_start:'
            printf '%s\n' '    nop'
            awk -v name="$name" '
                $0 == "case_" name ":" { emit = 1 }
                emit {
                    print
                    if ($0 ~ /^[[:space:]]+ret$/) {
                        ret_count++
                        if (ret_count == 2)
                            exit
                    }
                }
            ' "$HOST_WORK/probe.s"
        } >"$HOST_WORK/case-$name.s"
    done <"$HOST_WORK/expected-labels.txt"
}

cat >"$OUT_DIR/extract-objdump-bytes.awk" <<'EOF'
function flush() {
    if (label != "" && bytes != "") {
        print label "\t" bytes
        label = ""
        bytes = ""
    }
}

/^[[:xdigit:]]+ <(case|expect)_[^>]+>:/ {
    flush()
    label = $0
    sub(/^[^<]*</, "", label)
    sub(/>:.*/, "", label)
    next
}

label != "" && /^[[:space:]]*[[:xdigit:]]+:/ {
    bytes = ""
    for (i = 2; i <= NF; i++) {
        if ($i !~ /^[[:xdigit:]][[:xdigit:]]$/)
            break
        bytes = bytes (bytes == "" ? "" : " ") tolower($i)
    }
    flush()
}

END {
    flush()
}
EOF

check_mode_bytes() {
    local mode=$1
    local bytes_tsv=$2
    local labels_file=${3:-$HOST_WORK/expected-labels.txt}
    local failures=0
    local name actual expected

    while IFS= read -r name; do
        actual=$(awk -F '\t' -v key="case_$name" '$1 == key { print $2; exit }' "$bytes_tsv")
        expected=$(awk -F '\t' -v key="expect_$name" '$1 == key { print $2; exit }' "$bytes_tsv")
        if [[ -z $actual || -z $expected ]]; then
            printf '%s\t%s\tMISSING\tactual=%s\texpected=%s\n' "$mode" "$name" "$actual" "$expected" \
                >>"$OUT_DIR/failures.tsv"
            failures=$((failures + 1))
            continue
        fi
        if [[ $actual != "$expected" ]]; then
            printf '%s\t%s\tMISMATCH\tactual=%s\texpected=%s\n' "$mode" "$name" "$actual" "$expected" \
                >>"$OUT_DIR/failures.tsv"
            failures=$((failures + 1))
        fi
    done <"$labels_file"

    return "$failures"
}

run_split_mode() {
    local mode=$1
    local jit_value=$2
    local prefix=$OUT_DIR/$mode.split
    local guest_script=$GUEST_WORK/run-$mode-split.sh
    local failures=0

    split_cases
    cat >"$HOST_WORK/run-$mode-split.sh" <<EOF
#!/bin/sh
set -eu
cd "$GUEST_WORK"
if [ -w /proc/ish/amd64_jit ]; then
    echo "$jit_value" >/proc/ish/amd64_jit
fi
: >split.status
for src in case-*.s; do
    name=\${src#case-}
    name=\${name%.s}
    if as -o "case-\$name.o" "\$src" >"case-\$name.as.stdout" 2>"case-\$name.as.stderr"; then
        objdump -dr "case-\$name.o" >"case-\$name.objdump" 2>"case-\$name.objdump.stderr" || true
        echo "\$name OK" >>split.status
    else
        rc=\$?
        echo "\$name AS_FAIL \$rc" >>split.status
    fi
done
EOF
    chmod +x "$HOST_WORK/run-$mode-split.sh"

    ISH_HOST_AMD64_JIT=$jit_value "$ISH_BIN" -r "$ROOTFS" "${GUEST_SHELL_ARGV[@]}" "$guest_script" \
        >"$prefix.stdout" 2>"$prefix.stderr" || true

    cp "$HOST_WORK/split.status" "$prefix.status" 2>/dev/null || true
    : >"$prefix.bytes.tsv"
    : >"$prefix.ok-labels.txt"
    while IFS= read -r name; do
        if grep -q "^$name OK\$" "$HOST_WORK/split.status" 2>/dev/null; then
            cp "$HOST_WORK/case-$name.objdump" "$OUT_DIR/$mode.$name.objdump" 2>/dev/null || true
            awk -f "$OUT_DIR/extract-objdump-bytes.awk" "$HOST_WORK/case-$name.objdump" >>"$prefix.bytes.tsv"
            printf '%s\n' "$name" >>"$prefix.ok-labels.txt"
            continue
        fi
        cp "$HOST_WORK/case-$name.as.stderr" "$OUT_DIR/$mode.$name.as.stderr" 2>/dev/null || true
        printf '%s\t%s\tAS_FAIL\t%s\n' "$mode" "$name" "$(tr '\n' ' ' <"$HOST_WORK/case-$name.as.stderr" 2>/dev/null)" \
            >>"$OUT_DIR/failures.tsv"
        failures=$((failures + 1))
    done <"$HOST_WORK/expected-labels.txt"

    if [[ -s $prefix.bytes.tsv ]]; then
        set +e
        check_mode_bytes "$mode" "$prefix.bytes.tsv" "$prefix.ok-labels.txt"
        rc=$?
        set -e
        failures=$((failures + rc))
    fi

    if [[ $failures -ne 0 ]]; then
        echo "$mode: FAIL $failures split-case failure(s)"
        return 1
    fi
    echo "$mode: PASS split cases"
}

run_mode() {
    local mode=$1
    local jit_value=0
    local prefix=$OUT_DIR/$mode
    local guest_script=$GUEST_WORK/run-$mode.sh
    local rc=0

    case "$mode" in
        interp) jit_value=0 ;;
        jit) jit_value=1 ;;
        *)
            echo "unknown mode: $mode" >&2
            return 2
            ;;
    esac

    cat >"$HOST_WORK/run-$mode.sh" <<EOF
#!/bin/sh
set -eu
cd "$GUEST_WORK"
if [ -w /proc/ish/amd64_jit ]; then
    echo "$jit_value" >/proc/ish/amd64_jit
fi
as -o probe-$mode.o probe.s
objdump -dr probe-$mode.o >probe-$mode.objdump
EOF
    chmod +x "$HOST_WORK/run-$mode.sh"

    set +e
    ISH_HOST_AMD64_JIT=$jit_value "$ISH_BIN" -r "$ROOTFS" "${GUEST_SHELL_ARGV[@]}" "$guest_script" \
        >"$prefix.stdout" 2>"$prefix.stderr"
    rc=$?
    set -e

    cp "$HOST_WORK/probe-$mode.objdump" "$prefix.objdump" 2>/dev/null || true
    if [[ $rc -ne 0 ]]; then
        echo "$mode: combined probe failed with assembler exit $rc; falling back to split cases"
        run_split_mode "$mode" "$jit_value"
        return $?
    fi

    awk -f "$OUT_DIR/extract-objdump-bytes.awk" "$prefix.objdump" >"$prefix.bytes.tsv"

    local failures=0
    set +e
    check_mode_bytes "$mode" "$prefix.bytes.tsv"
    failures=$?
    set -e

    if [[ $failures -ne 0 ]]; then
        echo "$mode: FAIL $failures encoding mismatch(es)"
        return 1
    fi
    echo "$mode: PASS"
}

: >"$OUT_DIR/failures.tsv"
status=0
IFS=',' read -r -a mode_list <<<"$MODES"
for mode in "${mode_list[@]}"; do
    if ! run_mode "$mode"; then
        status=1
    fi
done

cat >"$OUT_DIR/README.txt" <<EOF
amd64 GAS probe output
======================

Rootfs: $ROOTFS
iSH binary: $ISH_BIN
Guest shell: $GUEST_SHELL
Guest work directory: $GUEST_WORK

Files:
  interp.objdump / jit.objdump      Guest objdump -dr output
  interp.bytes.tsv / jit.bytes.tsv  Extracted case/expect instruction bytes
  failures.tsv                      Encoding mismatches, if any
  *.stdout / *.stderr               iSH stdout/stderr for each mode

The source and object files are also left in:
  $HOST_WORK
EOF

if [[ -s $OUT_DIR/failures.tsv ]]; then
    echo
    cat "$OUT_DIR/failures.tsv"
fi
echo "amd64 GAS probe results: $OUT_DIR"
exit "$status"
