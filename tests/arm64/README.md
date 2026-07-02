# arm64 guest bring-up smoke tests

Hand-written AArch64 assembly, no libc — validates the interpreter
(`emu/arm64_interp.c`) directly against real hardware-assembled instruction
encodings, ahead of a real dynamic-loader/libc bring-up (which needs an
arm64 rootfs — not yet built, see `aarch64_guest_plan.md`'s Testing
Strategy). Different category from `tests/manual/*.c`, which are guest C
programs run through the full regression harness against a real rootfs.

Each test exits with a code that indicates success (0, or a documented
specific value); a crash, hang, or wrong exit code means a real bug, not a
harness problem.

| Test | Covers |
|------|--------|
| `arm64_hello.s` | write(2)+exit(2) via SVC; ADR, MOVZ. Expected: prints "hi", exits 42. |
| `arm64_prologue.s` | Function prologue/epilogue (STP/LDP pre/post-index), BL/RET, ADD-immediate (incl. the `mov xd, sp` alias), SUBS-immediate, B.cond, CBZ. Expected: exits 0. |
| `arm64_atomics.s` | LDXR/STXR (success case), CAS (success and expected-value-mismatch/failure cases). Expected: exits 0. |

Deliberately avoid register-form ADD/SUB (`add x0, x0, x1`) and the
register-form MOV alias (`mov x0, x1`) — both are DP_REG (logical/add-sub
shifted register), a documented patch-3 scope cut. Using them will hit
`INT_UNDEFINED`/SIGILL, which is expected until that scope lands, not a
regression in these tests.

## Building

Needs a target-aarch64-capable clang (any recent Xcode clang has the
backend) and LLVM's `lld` (Apple's system `ld` doesn't understand ELF
target flags — install via `brew install lld`):

```sh
clang -target aarch64-linux-gnu -fuse-ld=/opt/homebrew/opt/lld/bin/ld.lld \
    -nostdlib -static -Wl,-e,_start -o arm64_hello arm64_hello.s
```

## Running

Needs a CLI `ish` build (`meson setup build && ninja -C build ish`) and a
patch-5-or-later `aarch64` branch checkout (the `ENOEXEC` guard from patch 1
must be lifted). No fakefs/rootfs needed — run directly via realfs
passthrough:

```sh
./build/ish -r / /path/to/tests/arm64/arm64_hello
echo $?   # 42
```

## Bugs these tests actually caught

`arm64_atomics.s` caught a real ordering bug during initial patch-5
validation: CAS's bit pattern also matches the broader load/store-exclusive
family mask (both are in the same top-level "atomic" encoding group), and
since the LDXR/STXR check ran first in `arm64_execute`'s if-chain, it
intercepted CAS instructions and misclassified them as an unimplemented
STLR-family op, raising `INT_UNDEFINED` instead of executing the CAS. Fixed
by reordering the checks (CAS first), matching the order OpenMinis'
`asbestos/guest-arm64/gen.c` already uses for the same reason. This is
exactly the kind of bug hand-computed sanity checks (the earlier
non-memory-touching scratch verification) can't catch — only running real
encodings through the actual decode dispatch does.
