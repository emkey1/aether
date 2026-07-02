# arm64 guest bring-up smoke tests

Hand-written AArch64 assembly, no libc — validates the interpreter
(`emu/arm64_interp.c`) directly against real hardware-assembled instruction
encodings, ahead of a real dynamic-loader/libc bring-up. An arm64 rootfs
(Alpine 3.21 minirootfs) has since been used for exactly that bring-up
testing — see "Real-rootfs findings" below — but isn't checked into this
repo (it's a large binary download, not source; see that section for how
to fetch it). Different category from `tests/manual/*.c`, which are guest C
programs run through the full regression harness against a real rootfs.

Each test exits with a code that indicates success (0, or a documented
specific value); a crash, hang, or wrong exit code means a real bug, not a
harness problem.

| Test | Covers |
|------|--------|
| `arm64_hello.s` | write(2)+exit(2) via SVC; ADR, MOVZ. Expected: prints "hi", exits 42. |
| `arm64_prologue.s` | Function prologue/epilogue (STP/LDP pre/post-index), BL/RET, ADD-immediate (incl. the `mov xd, sp` alias), SUBS-immediate, B.cond, CBZ. Expected: exits 0. |
| `arm64_atomics.s` | LDXR/STXR (success case), CAS (success and expected-value-mismatch/failure cases). Expected: exits 0. |
| `arm64_logical.s` | Logical (immediate): AND, ORR (incl. the MOV-alias via Rn=XZR), EOR, ANDS. Expected: exits 0. |

Deliberately avoid register-form ADD/SUB (`add x0, x0, x1`) and the
register-form MOV alias (`mov x0, x1`) — both are DP_REG (logical/add-sub
shifted register), a documented patch-3 scope cut, confirmed still-missing
by the real-rootfs test below. Using them will hit `INT_UNDEFINED`/SIGILL,
which is expected until that scope lands, not a regression in these tests.

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

## Real-rootfs findings

Downloaded the official Alpine 3.21 aarch64 minirootfs
(`https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/aarch64/alpine-minirootfs-3.21.4-aarch64.tar.gz`,
not checked in — re-download and `tar -xzf` into a scratch dir) and tried
booting its real, dynamically-linked `/bin/busybox` (PIE, musl dynamic
linker) via `./build/ish -r <extracted-rootfs> /bin/busybox`. This is
expected to fail — patches 6 (VDSO) and 7 (TLS) aren't done, and the
dynamic linker needs both — but it's useful as a prioritized "what's
actually missing" signal instead of guessing:

- **First finding**: the very first instruction musl's `ld.so` executes is
  a Logical (immediate) instruction — not a rare case, the *literal first
  instruction* of any real dynamic-linked program. This directly motivated
  adding `arm64_decode_bitmask_imm`/Logical(immediate) support (see
  `arm64_logical.s` above) ahead of schedule; patch 3 had deferred it as
  lower-priority than it turned out to be.
- **Current blocker** (as of the Logical-immediate addition): a DP_REG
  instruction (`0xaa0003e8`, register-form `ORR`/`MOV` alias) two
  instructions later. DP_REG (logical-shifted-register, add/subtract
  shifted/extended register, the register-form `MOV` alias) remains
  patch 3's largest documented scope cut and is now confirmed to be the
  next real blocker for any dynamic-linked userland, not a hypothetical
  one.
