# iSH-AOK Remote Differential Test Harness

A crash-resilient, differential test harness for the iSH-AOK x86 emulator. It
runs each test across every **(arch × engine)** the emulator offers, plus a
**native x86 oracle**, and flags any divergence or crash — then minimizes it to
a repro. Built to push the edges of the 32- and 64-bit JIT.

## Status

| Piece | State |
|---|---|
| `conductor.py` — build → matrix → oracle → key-based compare → report | **working** |
| local-fakefs backend (host `./build/ish`) | **working** (primary) |
| Rosetta `arch -x86_64` oracle (x86_64) | **working** |
| mint Lima-VM oracle (true i386 + real-Linux x86_64) | **working** |
| `corpus/flags_alu.c` — integer ALU result+EFLAGS exactness | **working** |
| crash/hang classification (signal-exit / timeout) | **working** (local) |
| `minimize` (case bisection) | basic |
| ssh / devicectl device backend (journal + heartbeat) | designed, not built |
| corpus: shifts, SSE/cvt, rep-string, mem-operand adc/sbb, atomics, … | planned |

## Quickstart

```bash
# from repo root, after `ninja -C build`
python3 tests/remote/conductor.py run                 # all corpus tests, all cells
python3 tests/remote/conductor.py run --tests flags_alu
python3 tests/remote/conductor.py run --cells oracle,amd64:interp,amd64:jit
python3 tests/remote/conductor.py minimize --test flags_alu --cell amd64:jit
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
- **device (designed):** a JIT bug there also kills sshd and drops the
  connection. The recovery design:
  - a **guest supervisor** forks each test as a child and writes a
    `START …` / `END …` **journal** + a **heartbeat** to the guest fs (the
    fakefs SQLite / real dir persists across an app restart);
  - the conductor streams the journal; loss of heartbeat → probe port 1022;
  - on confirmed crash it reads the journal — the id with `START` but no `END`
    is the crasher — pulls forensics (iOS crash report, app log, `[amd64-jit]
    bad-*` diagnostics), then **recovers**: auto-relaunch via `xcrun devicectl`
    (USB-tethered), or notify + poll port 1022 (ssh-only);
  - the crasher is quarantined and the suite resumes; a later isolated-repro
    pass re-runs it with `ISH_TRACE_*` on and minimizes it.

Backends sit behind one interface (`launch / kill / is_up / pull_crashlog`).
The iOS **Simulator is intentionally unsupported** — the default
case-insensitive macOS volume corrupts Linux rootfs paths.

## Corpus

Two test styles flow through the same matrix:

- **Differential** (new, JIT-edge): print canonical `result+flags`; require
  byte-identical across cells + oracle. `corpus/*.c` via `diff_common.h`.
- **Self-checking** (existing `tests/manual/*.c`): exit 0 + `^<name>: PASS$`,
  required in every cell. (To be wrapped — Tier 0.)

Planned differential families (each guards a bug class this project has hit):
flags ✅, **shifts/rotates** (CF/OF at count 0/1/≥width), **SSE/cvt**
(out-of-range → integer-indefinite, NaN/Inf, shuf lanes, pmovmskb),
**rep-string** (cross-page, DF, 16-bit), **mem-operand adc/sbb** (the native
gadget path, *not* the bridged register path), **mul/div** (#DE), **atomics**
(cmpxchg8b/16b), **sign/zero-ext**, **control-flow/SMC**.

## First validated finding (worked example)

On its first run the harness flagged 214 divergent keys, all `adc`/`sbb` with
carry-in = 1, on **amd64 only** (i386 matched the oracle exactly). Root cause:
`amd64_set_adc_flags` / `amd64_set_sbb_flags` pre-folded the carry into
`rhs_with_carry = rhs + carry_in` and fed it to the **carry-less** AF/OF
formulas; the folded carry ripples past bit 3 (`0x7f+1=0x80`), corrupting the
bit-4 XOR (AF) and the signed-overflow test (OF). Fixed by using the original
`rhs` for OF and the carry-aware nibble form for AF. Harness verdict after the
fix: all cells agree. (The native *memory-operand* adc/sbb gadget uses the same
anti-pattern and is a corpus-expansion target.)

## Layout

```
tests/remote/
  conductor.py          orchestrator (build, matrix, compare, crash, minimize)
  corpus/
    diff_common.h       differential harness: flag capture, engine self-select,
                        --case/--seed/--list, canonical emit
    flags_alu.c         integer ALU result+EFLAGS exactness
  .work/                build artifacts + results.json (gitignored)
  README.md             this file
```
