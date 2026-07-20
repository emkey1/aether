# iSH-AOK performance optimizations — July 2026

This document summarizes the JIT and crypto optimizations landed in this cycle,
with the benchmarks that justify (or in a couple of cases, corrected) each one.

All work is on the `working` branch. Commits are noted per section.

## Measurement notes (read first)

Two facts shaped every benchmark here and are worth stating up front, because
each one produced a *wrong* first reading during development:

- **Measure at `-O2`, never the debug CLI.** The command-line `ish` is built
  `buildtype=debug, optimization=0`, so *all* emulator C — including the crypto
  reference code — runs unoptimized. Compute-bound numbers taken there
  understate the shipping (Release/`-O2`) app by 5–6×. The crypto-provider path,
  for example, measured **0.8×** at `-O0` and **15–19×** at `-O2`. For an honest
  number: `meson setup build-o2 -Doptimization=2 && ninja -C build-o2 ish`.
- **Single-run sub-second timings are noise.** An early sort benchmark read
  "20% slower" on one run and "22% faster" on a best-of-3 with a properly staged
  file. Stage large inputs in the persistent fakefs (not `/tmp`, which is
  tmpfs), keep input generation outside the timed region, and take best-of-N.

Guest images used: Devuan arm64 (glibc 2.41), Alpine arm64 (musl), Alpine
riscv64 (musl). Host: Apple Silicon.

---

## 1. arm64 JIT — ANDS/TST + B.cond fusion

**Commit:** `4067a8db` (PR #504), test `e70c32a9`.

Collapses an `ANDS`/`TST` followed by a conditional branch into a single fused
gadget dispatch, skipping the intermediate NZCV flag round-trip — the same
pattern already used for `CMP`/`SUBS`. `ANDS`+`B.cond` is the highest-impact
missing fusion (8–15% of conditional branches follow an ANDS test).

The PR initially shipped a bug (the TST-skip `cmp x9,#31` clobbered the NZCV
flags the branch reads); it was caught by the regression harness before merge
and fixed by the author. Validated by `tests/manual/arm64/ands_bcond_fusion.c`
(140 probes: all 14 condition codes × imm/reg × 32/64-bit × TST/normal-Rd).

---

## 2. amd64 JIT — native SSE gadgets for the top interpreter-bridge opcodes

**Commit:** `7532e8ea`.

The amd64 JIT bridged many hot SSE2 opcodes to the interpreter one instruction
at a time. `ISH_TRACE_AMD64_JIT_STATS=1` on a representative workload
(`ls -laR`, `md5sum`, `gzip` round-trip) showed **131,072** per-instruction
bridge crossings dominated by 13 SSE2 opcodes (glibc's SSE2 `str*`/`mem*` and
zlib's hot loops).

Added native aarch64-host gadgets for all 13, in register and memory-operand
forms: `paddw`, `psubusw`, `por`, the `punpckl*` family (via NEON `zip1`),
`pcmpeqb`, the `movq`/`movd` family, and `pshufd`/`pshuflw`/`pshufhw` +
`pslldq`/`psrldq` (one generic `tbl` gadget with the shuffle mask baked into
the code stream at translation time).

| metric | before | after |
|---|---|---|
| per-instruction SSE bridges (same workload) | 131,072 | 1,024 (**128×** fewer) |

Correctness: differential runs (full-range `ISH_AMD64_FORCE_INTERP` vs JIT) gave
bit-identical `md5`/`sha1`/`sha256`, `gzip -9` round-trips, and text-tool output
on two rootfs images; `amd64_regress` passes in-guest (compiled by the guest's
own gcc under the JIT). i386/arm64/riscv64 guests unaffected.

---

## 3. HLE — high-level emulation of hot libc functions (arm64 / riscv64)

**Commits:** `62895a14` (core), `643ea9eb` (direct-pointer fix), `740e361c`,
`e9277464`, `cce222b9` (function families), `8f83f6ab` (tracer). Default **OFF**
(`ISH_HLE=1` / app toggle).

When a translated block starts at the entry of a known libc function —
identified by an exact 64-byte prologue fingerprint against a table extracted
from the bundled rootfs libcs — the whole block becomes one gadget that runs a
host-native C implementation of the function directly over guest memory.

**22 functions covered:** `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`,
`memrchr`, `strlen`, `strnlen`, `strcmp`, `strncmp`, `strchr`, `strrchr`,
`rawmemchr`, `strspn`, `strcspn`, `strpbrk`, `strcpy`, `stpcpy`, `strncpy`,
`stpncpy`, `strcat`, `strncat`. 64 fingerprints across glibc-arm64,
musl-arm64, musl-riscv64.

### 3a. The direct-pointer correctness-of-approach fix (`643ea9eb`)

The first cut bounced guest memory through a 256-byte stack buffer via
`tlb_read`/`tlb_write` — double the traffic plus a TLB lookup per chunk — and
**benchmarked ~2× *slower* than the plain translated libc**, because the JIT
already writes directly to host memory via NEON. The fix resolves a direct host
pointer per guest page (`__tlb_read_ptr`/`__tlb_write_ptr`) and runs one native
libc call per page-span, no bounce buffer.

Microbenchmark (memcpy/memset/memcmp/strlen loop, best-of-3, HLE off→on):

| buffer | arm64 | riscv64 |
|---|---|---|
| 256 B | 1.23× | — |
| 4 KB | 3.16× | 2.01× |
| 64 KB | **7.17×** | 2.04× |
| 1 MB | 6.68× | 2.07× |

### 3b. Aggregate real-workload benchmark (`ISH_HLE` off vs on)

Microbenchmark wins don't tell the real story, so the full 22-function set was
measured on real workloads (best-of-3, 124 MB staged inputs, arm64):

| workload | HLE off | HLE on | delta |
|---|---|---|---|
| **sort** (4M records, heavy memcpy/memcmp) | 8.35 s | 6.49 s | **+22%** |
| base64 | 2.37 s | 2.25 s | +5% |
| wc | 1.72 s | 1.67 s | +3% |
| sort \| uniq | 8.14 s | 7.88 s | +3% |
| md5 / sha256 / tr / cut | — | — | neutral |

**Honest takeaway:** HLE is a meaningful win on **data-movement-heavy**
workloads (sort) and modest on several common ones, but neutral on compute- or
hash-bound tasks (which spend their time in application logic, not libc calls).
This is why it ships **off by default**. Correctness for the whole set was
validated by edge-case tests (identical off vs on, arm64-glibc and musl-riscv64)
plus real-program differentials bit-identical across all three images.

### 3c. Near-miss candidate tracer (`8f83f6ab`)

`ISH_HLE_TRACE=1` FNV-hashes unmatched block-start prologues and periodically
dumps the recurring ones, ranked — a data-driven way to find future fingerprint
candidates. Caveat learned in use: it ranks by *processes-touching*, not
*executions*, so on fork-heavy workloads it surfaces `ld.so` startup code, not
compute hot loops.

### 3d. riscv64 RVC alignment fix — four "covered" functions were never attaching (`2b11bc4b`)

`hle_try_emit` rejected any block start with `ip & 3` before consulting the
table, assuming 4-aligned function entries. riscv64 with RVC places entries at
2-byte alignment: Alpine's musl-riscv64 has `memcpy`, `strcmp`, `memchr` and
`strnlen` at 2-mod-4 addresses, so those fingerprints were dead entries — the
four hottest string functions had **never** run under HLE on riscv64 (a
tar-over-/usr battery that makes ~380k HLE memcpy calls on arm64 made zero on
riscv64). The gate is now `ip & (riscv64 ? 1 : 3)`. This one-liner, not any
missing table entry, was riscv64's real HLE coverage gap.

### 3e. Symbol-table attach — HLE now survives libc upgrades (`56cadc03`)

Fingerprints only match the exact libc builds they were extracted from; one
`apk upgrade musl` used to silently disable HLE. On a fingerprint miss, a
block start inside a mapping whose file looks like a libc
(`ld-musl-*` / `libc.musl-*` / `libc.so.6` / `libc-*`) is now matched against
the ELF's **dynamic symbol table**, parsed once per file through the mapping's
retained fd (`DT_HASH`/`DT_GNU_HASH`) and cached globally by a content hash of
the ELF header. The cache stores file-relative offsets only; the load bias is
recomputed from the live page tables on every lookup, so remaps can never act
on stale addresses. glibc `STT_GNU_IFUNC` entries are skipped (their
`st_value` is the resolver; the dlsym-captured fingerprints keep covering
those). `ISH_HLE_SYMTAB=0` disables; `ISH_HLE_FP=0` disables the fingerprint
table for testing — the arm64 guest regression suite passes symtab-only.

### 3f. Call statistics + named near-miss ranking (`56cadc03`)

`ISH_HLE_STATS=1` dumps per-function call counts and a size histogram at exit
(through a startup `dup` of stderr — guest teardown closes host fd 2 before
`cli_halt` runs), and in trace mode the near-miss ranking now resolves
candidate addresses to libc symbol **names** via the symtab machinery.
What the data settled:

- A 2.4M-call tar/find/grep/fork battery shows **no missing high-count string
  function** — the 22-function set is complete; only TLS/lock/syscall wrappers
  (`__errno_location`, `pthread_mutex_lock`, `mmap`) recur, a different class.
- Per-call bridge overhead is **zero vs translated code even at n=8** (and
  5–10× ahead at 4K), killing a planned small-n fast path.
- The whole 7-Zip benchmark makes only ~5.7k libc string calls — clearing HLE
  of a suspected SMP regression there (interleaved A/B confirmed: variance).

### 3g. Copy/fill loop idiom JIT, arm64 guest (`56cadc03`, test `7cbaedb3`)

A fingerprint can only catch a libc *call*; open-coded loops (inlined memcpy
at -O0/-O1, LZMA match copy, hand-rolled byte loops) have no entry to attach
to. But the backward branch makes the loop head a block start, so the same
hook recognizes 3–4-instruction self-loops — `LDR`/`STR` post-index with
`SUBS`+`B.NE`, `SUB`+`CBNZ`, or `CMP`+`B.NE` counters, plus store-only fills —
and turns the whole block into one bulk gadget (reusing the `hle_call` bridge,
zero new assembly). The executor preserves the loop's exact architectural
contract: registers advance per iteration, the data register holds the last
loaded element, NZCV matches the last executed `SUBS`/`CMP`, work is chunked
(1M iterations per entry, re-entering the block), zero counts wrap 2^64
do-while style, faults land on the precise iteration boundary (or
mid-iteration with pc on the store for a copy store-fault), and
forward-overlapping copies (LZ77 match with distance < length) reproduce
byte-forward propagation via periodic tiling — never memmove. 15 such loops
attach in a single 7z run (isolating them measures +3–13% on decompression);
musl's startup fill loop attaches in every process. Guarded by
`ISH_HLE_LOOPS=0`; exercised deterministically by the inline-asm guest test
`tests/manual/arm64/hle_loop.c` (all shapes, overlap distances 1/3/7/255,
chunking, PROT_NONE fault semantics), which passes identically with HLE off.
The riscv64 recognizer is deferred: RVC's 2-byte encodings make the pattern
space combinatorial, and riscv64's bigger win was 3d.

---

## 4. Crypto accelerator — host-native ChaCha20-Poly1305 (arm64 / riscv64)

**Commits:** `84e95739` (AEAD + syscall), `38f9239d` (scratch reuse),
`3a6f50cd` (streaming API), `5a21306f` (direct-pointer), `dfd42c7a` (raw stream),
`6a163c2f` (OpenSSL provider). Default **OFF** (`ISH_CRYPTO_ACCEL=1`, and
self-test-gated).

### Motivation

riscv64 has no vector unit in iSH's RV64GC guest (NEON is ARM-specific), so its
OpenSSL falls back to scalar C, JIT-translated one instruction at a time.
Measured emulated ChaCha20-Poly1305: **riscv64 ~4.4× slower than arm64**. Rather
than fingerprint libcrypto (stripped internal symbols, stateful Poly1305, worst
update-treadmill), the accelerator uses a paravirt approach: a guest issues a
private syscall (`ISH_SYS_AEAD` = `0xacc0`) and the host runs the cipher
natively over the guest's buffers.

### Correctness

A plain, auditable RFC 8439 reference implementation (`kernel/ish_accel_crypto.c`),
validated **before** being trusted:

- RFC 8439 self-test (ChaCha20 + Poly1305 + AEAD vectors), gates enablement.
- **3200** differential cases vs OpenSSL across every length boundary — 0 fails.
- Streaming forms fuzzed with **random span splits** (3000 cases) vs OpenSSL — 0 fails.
- In-guest: RFC vector + round-trip + tamper-reject + 260-case OpenSSL
  differential, PASS on arm64 and riscv64.

### Performance (whole-AEAD, 16 KiB records, Release/`-O2`)

| arch | emulated | accelerator | speedup |
|---|---|---|---|
| **riscv64** | 17 MB/s | 565 MB/s | **33.6×** |
| arm64 | 83 MB/s | 570 MB/s | 6.8× |

The accelerator runs at the raw host-crypto speed (~553 MB/s standalone),
confirming the **direct-pointer path** (`5a21306f`, via the new
`user_transform_two` in `kernel/user.c`) eliminates both buffer copies — with
copies it capped near 150 MB/s. `38f9239d` (per-thread scratch, no malloc on the
hot path) roughly doubled the pre-direct-pointer path.

---

## 5. Transparent ssh/scp acceleration — OpenSSL provider

**Commit:** `6a163c2f` (`opt/AOK/crypto/`). Default **OFF** (only active when the
accelerator is enabled; declines the algorithm otherwise, so OpenSSL falls back
safely).

OpenSSH's default `chacha20-poly1305@openssh.com` drives its keystream through
`EVP_chacha20` (the raw **stream** cipher, not the whole-AEAD EVP call), so the
ssh-accelerating path is: a raw ChaCha20 stream op in the accelerator (`dfd42c7a`,
`alg=1`, matched bit-exact to `EVP_chacha20` over 3000+3000 host cases) plus a
small OpenSSL 3 provider (`ish_provider.c`, ~250 lines) that offers `ChaCha20`
and implements it via the syscall.

Proven end-to-end in-guest at Release/`-O2` (riscv64, 16 KiB records):

| path | default | accelerated | speedup |
|---|---|---|---|
| explicit `EVP_CIPHER_fetch(provider=ish)` | 26 MB/s | 496 MB/s | **19.4×** |
| **transparent** via `openssl.cnf` — legacy `EVP_chacha20()`, ssh's exact call | 25 MB/s | 384 MB/s | **15.4×** |

Output is bit-identical to OpenSSL's default provider (200-case differential).
The transparent path requires **no changes to ssh** — ship the provider `.so`
plus an `openssl.cnf` snippet (both in `opt/AOK/crypto/`, with `build-provider.sh`
and a README).

### Scope / follow-ons

- ChaCha20 **stream** is done end-to-end. A ChaCha20-Poly1305 **AEAD provider**
  (the AEAD crypto is already validated — just provider boilerplate) and
  **AES-256-GCM** (new, from-scratch AES+GHASH — the largest and most
  security-critical piece) are planned.
- The provider is correct for ssh's one-init-one-cipher pattern and 64-byte
  aligned streaming updates; a mid-stream sub-block-straddling update poisons the
  context (the accelerator declines rather than returning wrong data).
- Productization gaps: the provider must be built per guest-arch and shipped in
  the rootfs, and the app-side crypto-accelerator enable toggle isn't wired yet
  (CLI `ISH_CRYPTO_ACCEL=1` only).

---

## Summary table

| optimization | arch | headline | default |
|---|---|---|---|
| ANDS+B.cond fusion | arm64 | fewer dispatches on branch-dense code | on |
| amd64 SSE gadgets | amd64 | 128× fewer interpreter bridges | on |
| HLE libc (22 fns) | arm64/riscv64 | sort +22%, memcpy micro up to 7× | off |
| HLE symtab attach + riscv64 RVC fix | arm64/riscv64 | survives libc upgrades; riscv64 memcpy/strcmp/memchr/strnlen finally live | off (with HLE) |
| HLE copy-loop idiom JIT | arm64 | bulk gadget for open-coded copy/fill loops; 7z decode +3–13% | off (with HLE) |
| Crypto accelerator (AEAD) | arm64/riscv64 | riscv64 33.6×, arm64 6.8× | off |
| ChaCha20 via OpenSSL provider | arm64/riscv64 | ssh cipher ~15× transparently (riscv64) | off |
