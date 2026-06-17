# iSH-AOK Remote Differential Test Harness

A crash-resilient, differential test harness for the iSH-AOK x86 emulator. It
runs each test across every **(arch × engine)** the emulator offers, plus a
**native x86 oracle**, and flags any divergence or crash — then minimizes it to
a repro. Built to push the edges of the 32- and 64-bit JIT.

## Status

| Piece | State |
|---|---|
| `conductor.py` — 4 modes: `run` / `supervise` / `device` / `tier0` | **working** |
| local-fakefs backend (host `./build/ish`, device-identical aarch64 gadgets) | **working** (primary) |
| Rosetta `arch -x86_64` + mint Lima-VM oracles (true i386 + real-Linux x86_64) | **working** |
| differential corpus — 9 families (ALU, adc/sbb-mem, shifts, sign-ext, mul/div, mxcsr, bit-ops, rep-string, sse-cvt) | **working** |
| Tier 0 — the 21 `tests/manual` self-check tests | **working** (20/20 i386, 21/21 amd64) |
| `mint:i386:jit` cell — iSH built in mint's VM (x86_64-host i386 JIT) | **working** |
| crash/hang classification + journal reconciliation (`supervise`) | **working** (local-validated) |
| device backend — ssh deploy/run + devicectl/notify recovery | **scaffold** (dry-run-verified; needs a live device) |
| `minimize` (case bisection) | basic |

## Quickstart

```bash
# from repo root, after `ninja -C build`
python3 tests/remote/conductor.py run                  # differential corpus, all cells
python3 tests/remote/conductor.py run --tests flags_alu --cells oracle,amd64:jit
python3 tests/remote/conductor.py tier0                # tests/manual self-check suite, per arch
python3 tests/remote/conductor.py supervise            # journaled batch (device crash model), local
python3 tests/remote/conductor.py minimize --test flags_alu --cell amd64:jit
python3 tests/remote/conductor.py run --cells mint:i386:jit   # corpus under iSH built in mint's VM
python3 tests/remote/conductor.py device --dry-run --device-host <ip>   # device backend (scaffold)
# results.json + built artifacts land in tests/remote/.work/
```

Requires: arm64 macOS (so `./build/ish` runs the **device-identical aarch64
gadgets**), `zig`, Rosetta (`arch -x86_64`), `build/ish`, `build/tools/fakefsify`.

## How it works

Each corpus test is compiled **three ways from one source**:

| Build | Command | Runs under |
|---|---|---|
| i386 ELF | `zig cc -target x86-linux-musl -static` | iSH i386 |
| x86_64 ELF | `zig cc -target x86_64-linux-musl -static` | iSH amd64 |
| x86_64 Mach-O | `cc -arch x86_64` | `arch -x86_64` (oracle) |

The test executes an x86 instruction via inline asm, captures the result and the
architecturally **defined** EFLAGS bits, and prints one canonical line per case.
It bakes in **no** expected answers — the oracle is ground truth.

**Cells** (engine selection is runtime, via `/proc/ish`, so one `ish` build
covers all of them):

| Cell | Selection | Role |
|---|---|---|
| `oracle` | native x86_64 (Rosetta Mach-O) | x86_64 ground truth |
| `mint:x86_64` | x86_64 ELF in mint's Lima Linux VM | real-Linux x86_64 truth |
| `mint:i386` | i386 ELF in mint's Lima Linux VM | **true i386 truth** |
| `amd64:interp` | `echo 0 > /proc/ish/amd64_jit` | amd64 interp |
| `amd64:jit` | `echo 1 > /proc/ish/amd64_jit` | amd64 JIT |
| `i386:jit` | default | i386 JIT |
| `i386:no_cache` | `…/i386_no_cache_comm` | force fresh gadget regen |
| `i386:single_step` | `…/i386_single_step_comm` | one-instruction blocks |
| `mint:i386:jit` | iSH built in mint's VM (`gadgets-x86_64`) | x86_64-host i386 JIT — codegen independent of the M5/device aarch64 gadgets |
| `device:*` | iSH on a real device over ssh:1022 | the actual target (scaffold) |

The last two are **opt-in** (not in the default set): add `--cells mint:i386:jit`
or use the `device` subcommand explicitly.

**i386 has no interpreter**, so its ground truth is (a) the 3-mode self-diff
(`jit ≡ no_cache ≡ single_step`, which needs no oracle), (b) the x86_64 oracle
for width-agnostic values, and (c) the **`mint:i386`** Lima-VM oracle — true
i386 Linux ground truth. The M5 can't provide that itself (macOS dropped 32-bit;
Rosetta is x86_64-only), so the Intel laptop "mint" runs the *identical* static
i386 ELF in its x86_64 Linux VM (kernel IA32 compat; no 32-bit userspace needed)
via the read-only virtiofs mount of its home — a plain `scp` makes the binary
instantly executable in the VM. mint cells auto-skip when it's offline, so the
M5 runs standalone. Config: `ISH_MINT_HOST` (mint), `ISH_LIMA_INSTANCE` (ish),
`ISH_MINT_BINDIR` (`.ish-oracle/bin`).

**Comparison is key-based**, not `diff`: each line is `(key=op+width+operands) →
(value=res+flags)`. Cells are compared per key, so an i386 cell legitimately
omitting 64-bit lines is not a false positive. Per-op **defined-flag masks**
ensure undefined bits (e.g. AF after a logical op) are never compared.

## Crash resilience

A JIT bug kills the whole emulator (host SIGSEGV/SIGILL/abort). That is the
primary signal this harness exists to catch.

- **local-fakefs:** the host `ish` process *is* the device. A signal exit →
  `CRASH`; a timeout → `HANG`; both are captured with stderr. One crashing case
  never blocks the rest (and `minimize` bisects `--case` to the culprit).
- **device (scaffold — dry-run-verified, needs a live device):** a JIT bug
  there also kills sshd and drops the connection. The recovery model (the local
  `supervise` mode already validates the journal half):
  - `guest_supervisor.c` forks each test as a child and writes a
    `SUPER-START …` / `SUPER-END …` **journal** (to stdout *and* an fsync'd file)
    plus a **heartbeat** to the guest fs (the fakefs SQLite / real dir persists
    across an app restart, so the journal survives the ssh stream truncating);
  - the conductor streams the journal; loss of heartbeat → probe port 1022;
  - on confirmed crash it reads the journal — the id with `SUPER-START` but no
    `SUPER-END` is the crasher — pulls forensics (iOS crash report, app log,
    `[amd64-jit] bad-*` diagnostics), then **recovers**: auto-relaunch via
    `xcrun devicectl` (USB-tethered), or notify + poll port 1022 (ssh-only);
  - the crasher is quarantined and the suite resumes; a later isolated-repro
    pass re-runs it with `ISH_TRACE_*` on and minimizes it.

Backends sit behind one interface (`launch / kill / is_up / pull_crashlog`).
The iOS **Simulator is intentionally unsupported** — the default
case-insensitive macOS volume corrupts Linux rootfs paths.

## Corpus

Two test styles flow through the same matrix:

- **Differential** (`run`, JIT-edge): `corpus/*.c` via `diff_common.h` print a
  canonical `result+flags` line per case; require byte-identical across cells +
  oracle. Nine families so far, each guarding a bug class this project has hit:
  - `flags_alu` — integer ALU result + EFLAGS exactness
  - `adc_sbb_mem` — carry-in adc/sbb on the **native memory-operand gadget** (not
    just the bridged register path)
  - `shifts` — shl/shr/sar/rol/ror CF/OF at counts 0 / 1 / ≥ width
  - `sext` — sign/zero-extension (movsx/movzx/cbw/cwde/cdqe)
  - `muldiv` — mul/imul/div/idiv, high half + `#DE`
  - `sse_mxcsr` — ldmxcsr/stmxcsr control-word round-trip (amd64)
  - `bit_ops` — bt/bts/btr/btc (CF) and bsf/bsr (ZF, undefined-dest)
  - `rep_string` — rep movs/stos + repe/repne cmps/scas; DF fwd/back, cross-page
    spans (the batch fast path), ECX=0, 16-bit forms, early-out ECX + flags
  - `sse_cvt` — SSE/SSE2 add/sub/mul/div/sqrt/min/max (scalar + packed) and the
    conversions (cvtsi2/cvtt*2si/cvtss2sd/cvtsd2ss/cvtdq2ps); NaN results and
    out-of-range float→int are canonicalized so the documented arm64-vs-x86 NaN
    sign / saturation differences don't false-diverge
- **Self-checking** (`tier0`): the 21 `tests/manual/*.c` tests (atomics, futex,
  signals, ptrace, epoll, fcntl/OFD, copy_file_range, pidfd, …) built static and
  run under iSH per arch, gated on `^<name>: PASS$`. No oracle — functional
  regression, not differential.

Still planned: **SSE shuffle/blend** (shufps/unpck lane ordering, pmovmskb),
**atomics** (cmpxchg8b/16b), **control-flow/SMC**.

This run the harness found and fixed **12 amd64/i386 JIT flag & result bugs** —
adc/sbb carry-in AF/OF (interp *and* the native gadget); rol/ror CF on full
turns; ror-by-1 OF; sar CF past width; cbw sign-extend; 32-bit mul/imul high
half; 2-op imul w64 overflow; i386 16-bit imul; i386 div/idiv `#DE`; missing
amd64 LDMXCSR/STMXCSR; and i386 min/max returning the wrong operand on the
`+0`/`-0` (and NaN) tie — each confirmed against the oracle (Rosetta + real-Intel
mint) and validated back to green.

## First validated finding (worked example)

On its first run the harness flagged 214 divergent keys, all `adc`/`sbb` with
carry-in = 1, on **amd64 only** (i386 matched the oracle exactly). Root cause:
`amd64_set_adc_flags` / `amd64_set_sbb_flags` pre-folded the carry into
`rhs_with_carry = rhs + carry_in` and fed it to the **carry-less** AF/OF
formulas; the folded carry ripples past bit 3 (`0x7f+1=0x80`), corrupting the
bit-4 XOR (AF) and the signed-overflow test (OF). Fixed by using the original
`rhs` for OF and the carry-aware nibble form for AF. Harness verdict after the
fix: all cells agree. (The native *memory-operand* adc/sbb gadget had the same
anti-pattern; `adc_sbb_mem` was added to cover it and it too is now fixed.)

## Layout

```
tests/remote/
  conductor.py          orchestrator: run / supervise / device / tier0
  guest_supervisor.c    in-guest batch runner (journal for device crash recovery)
  corpus/
    diff_common.h       differential harness: flag capture, engine self-select,
                        --case/--seed/--list, canonical emit
    flags_alu.c adc_sbb_mem.c shifts.c sext.c muldiv.c sse_mxcsr.c bit_ops.c
  .work/                build artifacts + results.json (gitignored)
  README.md             this file
```
