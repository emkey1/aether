# Persistent Translated-Code Cache — Implementation Plan

Status: PLANNED (scoped 2026-07-23). Owner: unassigned. Companion plan:
`pixman_accel_plan.md` (steady-state rendering; this plan is cold start).

## 1. Problem and evidence

Every process translates its entire code footprint from scratch and throws the
result away at exec/exit. For interpreter/GUI stacks this dominates launch:
`avahi-discover` (Python + GTK3) takes 20–30 s to first window on the A10X iPad;
a 9-module Python stdlib import chain takes ~2.1 s even on an M-series Mac under
the -O2 CLI (measured 2026-07-23, `build-o2/ish`, Arch arm64 guest). The same
libraries (libc, libpython, GTK/GLib/Pango/cairo) are re-translated for every
process and every launch, byte-identical each time.

HLE was measured to NOT help this (symtab attach actually cost +15–30% on cold
start; fingerprint-only was neutral). The durable fix is to stop re-translating
unchanged file-backed code: persist translated blocks keyed by file content +
offset, reload them in any process that maps the same file.

Payoff ceiling to validate in Phase 0: the fraction of cold-start wall time
spent inside block translation (decode + gen). If that fraction is under ~30%
on device, this project should be re-scoped before building the heavy parts.

## 2. What a block is today (constraints)

- `struct jit_block` (jit/jit.h): header + `unsigned long code[]`, a flat
  stream mixing (a) host function pointers to pre-compiled gadgets, (b) literal
  parameters (immediates, register numbers, sizes), (c) absolute guest
  addresses (branch targets, fault/return ips). One jit per address space
  (`struct jit` hangs off the mmu); blocks are found via a hash keyed by guest
  ip plus a per-page list used for invalidation.
- All emission funnels through a single primitive: `gen(struct gen_state *,
  unsigned long thing)` (jit/gen.c:310). This is the load-bearing seam: a
  parallel *tag stream* can classify every emitted slot at emission time
  without touching the hundreds of per-instruction emitters.
- Block chaining patches `jump_ip` slots at runtime to other blocks' host
  `code[]` addresses (jit.c ~1476/1797, store-release). Chaining state is
  runtime-only and must NOT be serialized; `old_jump_ip[]` holds the unpatched
  values, and blocks are always insertable unchained.
- `hle_try_emit` (jit.c ~1053) can replace a block with an hle_call gadget;
  HLE attachment depends on runtime toggles, so cached blocks must record
  whether they were HLE blocks and under which flags.
- Host gadget addresses change every app launch (ASLR): serialized blocks must
  store stable gadget IDs, not pointers.
- Guest load bias changes per process (PIE/ASLR in the guest): embedded guest
  addresses must be rebased by (new_bias − old_bias). Addresses decoded from
  the instruction stream are module-relative by construction (ip-relative
  branches made absolute), so a single per-mapping delta suffices. Anything
  not provably intra-module (none known today; verify in Phase 1) forces the
  block to be marked non-cacheable.

## 3. Architecture

### 3.1 Cache key
A cache *bundle* is keyed by:
- guest arch + engine ABI version (a build id string: git hash of jit/ + emu/,
  regenerated at build time — any emitter change invalidates everything),
- feature flags that alter emission (HLE on/off + hle-table version, multicore
  on/off if it changes emission, trace/instr flags → never cache),
- identity of the backing FILE: content hash. For fakefs, hash the host
  backing file lazily and memoize in a new `ish_jitcache` sqlite table (or
  xattr-style column on `ish_stat`) invalidated on any write through fakefs.
  For realfs, hash on first use per boot, revalidate by (size, mtime) and
  rehash on mismatch. Anonymous or written-to (COW'd) pages are never cached.

### 3.2 On-disk format
One bundle file per (file hash, arch, engine version):
`<cache dir>/<engine-ver>/<arch>/<file-hash>.jbc`, containing:
- header (magic, version, counts, whole-file checksum),
- a page index: file-relative page offset → list of serialized blocks,
- per block: file-relative entry offset, end offset, flags (hle, arm64/riscv),
  code length, then the code stream as (tag, value) pairs where tag ∈
  {GADGET_ID, LITERAL, GUEST_ADDR_REBASE}; GUEST_ADDR_REBASE values are stored
  module-relative (guest addr − mapping base at record time).
CLI cache dir: `~/.cache/ish-jitcache` (override `ISH_JITCACHE_DIR`). App:
`Library/Caches/jitcache` in the container (purgeable by iOS — acceptable).

### 3.3 Gadget registry
Build-time generated table `jit/gadget_registry.inc`: every gadget symbol that
can appear in `code[]` gets a stable dense ID (order = registry file order,
appended only). Runtime builds two maps at startup: id → host pointer (array)
and host pointer → id (hash, only needed when recording). Registry generation
script walks the same tables gen.c uses (`gadgets.h` arrays for each backend)
so it cannot drift silently; a startup assert cross-checks counts.

### 3.4 Record path
When `ISH_JITCACHE=record` (or "on" = record+use): `gen()` also appends a tag
byte to `state->tags`. Classification:
- literals: default;
- gadget pointers: emitters push them via the same `gen()` — recognize by
  pointer-to-id hash hit at record time (no emitter changes needed);
- guest addresses: cannot be distinguished from literals by value. gen.c gets
  a second primitive `gen_addr(state, guest_addr_t)` and the (small) set of
  call sites that emit guest addresses is migrated to it — grep-auditable:
  jump targets, ip constants for exits/faults, HLE claim ranges. Phase 1's
  differential harness exists precisely to catch a missed site.
A background writer (one thread, low priority) drains completed blocks
per (file, page) into bundles at process teardown and on a size trigger; a
crash mid-write only loses cache (bundles are written to a temp name and
renamed, checksummed).

### 3.5 Load path
On translation miss (jit.c compile loop, before decode): if the faulting ip
falls in a clean file-backed executable mapping whose file has a bundle,
deserialize all blocks for that page: map gadget IDs → pointers, add the
mapping's bias to GUEST_ADDR_REBASE slots, insert into the jit hash unchained.
Deserialization is a memcpy plus one pass of fixups — orders of magnitude
cheaper than decode+gen. Blocks from the bundle that later get invalidated
(page written) simply die through the existing invalidation path; the bundle
is not touched (it describes the *file*, which didn't change).

### 3.6 Safety
- Never cache: pages modified since map (COW'd), anonymous mappings, guest
  JITs, blocks that crossed into a different mapping, blocks with any slot the
  recorder couldn't classify.
- Whole-bundle checksum + per-block bounds validation on load; any anomaly →
  drop bundle, log once, fall back to translation (never crash on bad cache).
- `ISH_JITCACHE=0/1/record/paranoid` — paranoid translates AND loads, then
  memcmp()s the streams (CI mode; the core Phase 1/2 validation tool).

## 4. Phases

### Phase 0 — measure the ceiling (1–2 days)
Add `ISH_JIT_TIMING=1`: accumulate wall time inside the compile path (decode+
gen+insert) per process, dump at exit. Run on device (or -O2 CLI with a CPU
handicap) for: python3 boot, the 9-import chain, bssh, avahi-discover. Deliver:
translation share of cold start + bytes/blocks per library. GO/NO-GO gate:
translation share ≥ ~30% on device-class hardware.

### Phase 1 — in-process round-trip (1–1.5 weeks)
Gadget registry + generator + startup maps. Tag stream in `gen()` +
`gen_addr()` migration for the arm64-guest backend ONLY (the Wayland/GTK arch;
i386/amd64/riscv64 explicitly deferred, non-arm64 guests bypass the cache).
Serialize/deserialize in memory; `paranoid` differential over the arm64
regression suite (`setup-regressions.sh` binaries) — every block stream must
round-trip bit-identically modulo rebase slots. No disk, no keying yet.

### Phase 2 — on-disk cache, keying, rebase (1.5–2 weeks)
Bundle format + writer thread + loader integration; file-hash plumbing for
fakefs (sqlite memo) and realfs; bias rebase (record module base via the
mapping's `data->file_offset`/vma start, already available at translation
time); engine-version string generation in the build. Validation: full arm64
regression suite + GTK/python workloads with cache cold→warm→hot; `paranoid`
sweep; kill -9 during write; corrupt-bundle fuzz (truncate/bit-flip → must
fall back cleanly). Measure: warm-cache python import chain and bssh launch
vs baseline (target ≥2x on translation-dominated launches per Phase 0 data).

### Phase 3 — app productization (1 week)
Settings toggle (default OFF initially; flip after a soak), cache dir in the
container + size cap w/ LRU by bundle atime (default 256 MB), "Clear JIT
cache" row, invalidation on root import/update (content hash already covers
it — the row is for disk space), multicore soak (the stress suite with cache
hot), device measurement on the iPad: avahi-discover/bssh time-to-window.

### Phase 4 (optional, later)
riscv64 + amd64 backends (repeat the `gen_addr` audit per backend); intra-
bundle block pre-chaining hints; shipping pre-warmed bundles for the bundled
rootfs' libc/busybox.

## 5. Risks
- **Unclassified guest-addr slots** → wrong execution after rebase. Mitigated
  by the paranoid differential (bit-compare vs fresh translation both with
  ASLR biases varied) and by defaulting any doubt to "don't cache the block".
- **gen.c churn**: every future emitter change must keep the tag discipline;
  the engine-version key makes mistakes at worst a silent cache miss, and
  paranoid mode in CI catches tag drift.
- **Fakefs write-detection**: the content-hash memo must be invalidated on
  every write path (fakefs mutators are centralized enough: fd write/truncate/
  mmap-writeback on the backing file). Realfs relies on size+mtime — document
  as best-effort, rehash on mismatch.
- **Payoff risk**: if Phase 0 shows translation is a minor share (plausible if
  interpreter arithmetic dominates), stop at Phase 0 and re-aim at the pixman
  provider / rendering path instead.

## 6. Effort
Roughly 4–6 weeks end-to-end for arm64-guest-only through Phase 3, heavily
front-loaded with validation tooling. Phase 0 is cheap and decisive — do it
first, alone.
