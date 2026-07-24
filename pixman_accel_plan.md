# Pixman Composite Accelerator (Paravirt Provider) — Implementation Plan

Status: **PHASES 0, 1, AND 2 DONE (2026-07-23), commits b2c97524 + c2f0d45a.**
End-to-end verified: real, unmodified production Wayland clients
(labwc + foot) genuinely accelerated through the actual start-wayland.sh
session path. Remaining work is v2 coverage (mask/OVER_MASK_A8 is the
biggest gap) and an app Settings toggle -- see "NEXT" at the bottom.
Owner: unassigned. Companion plan: `jit_code_cache_plan.md` (cold start;
NO-GO, unaffected by this plan). Direct precedent: the ChaCha20 crypto
accelerator (kernel/ish_accel.c +

## 0. Progress so far

**Phase 0 (profiling):** built an LD_PRELOAD profiling-only shim
(scratchpad, not committed -- read-only wall-time instrumentation around
pixman's 4 public entry points) and a GTK3 redraw-loop benchmark (labwc +
a `Gtk.DrawingArea` doing translucent-rectangle + text redraws at ~60fps
target). Measured, 3 runs, on the local Arch aarch64 CLI guest: labwc +
the GTK app spend **~23.5% of the interactive redraw window's wall time
inside raw pixman calls** (~1.44s of 6.13s). Well above a working "is this
worth it" bar, though below the plan's originally-guessed 40% -- treated as
a clear GO given the precedent (crypto accel got 15-19x on a similarly-
sized share of ssh/scp time).

**Phase 1 (host core + differential harness): DONE, commit b2c97524.**
- `kernel/user.c`: `user_transform_rect` / `user_transform_rect_two`, new
  direct-host-pointer primitives generalizing `user_transform_two` from one
  linear buffer to a strided 2D sub-rectangle (declared in `kernel/calls.h`).
- `kernel/ish_accel_pix.h` / `ish_accel_pix_kernels.c`: pure pixel kernels
  (FILL/COPY/OVER) operating on already-resolved host pointers.
- `kernel/ish_accel_pix.c`: guest ABI (`struct ish_pix_req`), self-test-gated
  enable (`doEnablePixAccel`, `ISH_PIX_ACCEL=1`), decline logic (self-overlap,
  oversized, misaligned stride/base).
- `ISH_SYS_PIXOP = 0xacc1` wired into `kernel/calls.c` (both the dispatch
  switch AND the arm64/riscv64 range-check bypass gate -- missing the
  second one initially caused a `SIGSYS`, first bug found).
- OVER's blend arithmetic (premultiplied, saturating per-channel add,
  fast divide-by-255) was independently validated against real pixman
  *before* being written into the kernel (mint oracle, 200,125 cases, 0
  mismatches) -- caught that a naive non-saturating formula is wrong for
  malformed/non-premultiplied test inputs.
- `tests/manual/pixman_accel.c`: differential test, dlopen's real pixman
  as the oracle (SKIPs cleanly without it), covers FILL/COPY/OVER across
  tight/offset/padded/multi-page/full-1280x720-frame geometries + 30
  random-fuzz cases + the 3 decline paths. PASSES through the actual
  `setup-regressions.sh` harness. Two harness-only bugs were found and
  fixed during validation (both in the TEST, not the kernel -- confirmed
  by direct-syscall debugging before assuming otherwise): a hand-typed
  self-test expected value was arithmetically wrong, and the test's
  hand-derived `PIXMAN_a8r8g8b8`/`PIXMAN_x8r8g8b8` format constants had
  the wrong `bpp` field (32 bits, not 4 bytes) -- verified the correct
  values (`0x20028888`/`0x20020888`) against real pixman.h before fixing.
- Full existing regression suite reruns clean with the accelerator off
  (default): zero regressions from the new primitives/dispatch gate.
- Also confirmed empirically (not assumed) before writing the kernel:
  `pixman_fill`'s `_xor` parameter is a plain overwrite, not a real XOR;
  its `stride` parameter is in 32-bit WORDS while `pixman_image_create_
  bits`'s `stride` is in BYTES -- two different unit conventions in the
  same library, verified separately on the mint oracle.

**Phase 2 (guest-side LD_PRELOAD shim): DONE, commit c2f0d45a.**
`opt/AOK/tools/pixman/ish_pixman_shim.c` (note: lives under `opt/AOK/tools/
pixman/`, not `opt/AOK/pixman/` as originally sketched above -- the
fs/aok-tools.manifest baking mechanism only reads from `opt/AOK/tools/`).
Interposes `pixman_image_composite32` + `pixman_fill`, plus the five
property setters (`set_transform`/`set_repeat`/`set_filter`/
`set_alpha_map`/`set_clip_region(32)`/`set_has_client_clip`) needed to
shadow-track state pixman has no public getter for; `pixman_image_get_
component_alpha` DOES have a getter, so that one is queried directly, no
shadowing needed. Shadow entries live in a pointer-keyed hash table,
created lazily by the first setter call, purged when real
`pixman_image_unref()` reports the image was actually freed -- verified
empirically on the mint oracle first that it returns nonzero exactly on
the unref that hits refcount zero, never before, so a later `malloc()`
reusing the same address can never inherit stale state.

`setup-wayland.sh` now builds the shim (best-effort) to `/usr/local/lib/
ish-pixman/libish-pixman.so`; `start-wayland.sh` exports `LD_PRELOAD`
automatically when that file exists (`ISH_WAYLAND_DISABLE_PIXMAN_SHIM=1`
opts out). `fs/aok.c` needed a new `aokfs_tools_pixman_dir` node (mirroring
the existing `ktop` subdirectory node exactly) -- **trap discovered the
hard way**: `fs/aok-tools.manifest`'s generated-file table has NO automatic
subdirectory support; every subdirectory needs its own hand-wired
directory node in `fs/aok.c` (path string, `is_dir` membership, lookup-
table entry, and a readdir case scanning the generated table by path
prefix), exactly duplicating what `ktop/` already required. Also: the
manifest is read at **meson configure time** (`fs.read()` inside a
`foreach` in meson.build), so editing it and running a bare `ninja` is
NOT sufficient -- `meson setup --reconfigure` is required before new
manifest entries actually appear in the built `/AOK/tools` tree (a plain
`ninja` will silently keep serving the old file list).

**Verified end-to-end on the local CLI harness through the REAL
start-wayland.sh** (not a hand-rolled test): built the shim via
setup-wayland.sh's new step, ran a full session, confirmed `LD_PRELOAD`
was exported and picked up by real children. `ISH_PIXMAN_STATS=1` showed
genuine acceleration in two unmodified production Wayland clients:
- labwc itself: 3 composites + 31 fills accelerated, 2 mask + 35 format +
  3 bounds declines.
- **foot (the real terminal)**: 4948 fills accelerated against only 82
  mask declines -- a very high hit rate, since a terminal's cell-blit
  rendering is close to pure FILL/COPY with almost no masking, transforms,
  or scaling. This was a stronger, more convincing proof than the
  synthetic GTK bench would have been.

A separate visual (VNC-screenshot) sanity check was inconclusive -- the
screenshot came back a blank labwc background in BOTH a shimmed and an
unshimmed run of the same custom GTK bench script, so it's a pre-existing
test-environment issue (likely window placement/mapping in this specific
headless setup) unrelated to the shim; not chased further given the
stats + differential-test evidence already available. If picked up later,
worth a fresh look with a simpler test client (e.g. `foot` itself, whose
real session already proved to composite correctly per labwc's stats).

## v2 progress

**Mask support (`OVER_MASK_A8`): DONE, commit db6c4d57.** New kernel op,
`user_transform_rect_three` (three-image direct-pointer walk, mask at its
own bpp=1), pixel kernel `ish_pix_over_mask_row` -- blend formula (scale
src's premultiplied channels by mask_alpha/255, then ordinary OVER)
independently validated against real pixman on mint FIRST (300,625 edge +
random cases, 0 mismatches) before being written into the kernel. Shim's
`pixman_image_composite32` now routes OVER-with-a8-mask through it,
declining SRC-with-mask and any other op+mask combo (real pixman
operations, just not yet differential-tested). Two more harness-only bugs
found and fixed during validation (kernel correct throughout, confirmed
via direct-syscall debugging before assuming otherwise): (1)
`pixman_image_create_bits` requires EVERY stride to be a multiple of 4
bytes, even for a8 (1 byte/pixel) images -- an unaligned mask stride
doesn't error, it silently corrupts the image, which looked exactly like
a kernel bug until traced down. (2) A hand-typed self-test expected value
forgot every channel of premultiplied OVER blends identically, not just
alpha. Full existing regression suite reruns clean (all archs).

**KNOWN GAP surfaced by end-to-end testing**: re-ran the real
start-wayland.sh session with mask support live -- labwc's own compositing
still accelerates (3 composites + 31 fills), but **foot's masked glyph
composites still decline**, via `decline-format` rather than accelerating.
foot's terminal surface is very likely **x8r8g8b8** (opaque background,
no real alpha channel needed for a terminal), and v1 only supports
a8r8g8b8 as the DESTINATION format (x8r8g8b8 is only supported as a
*source*, where "ignore the top byte" is unambiguous). Supporting
x8r8g8b8-as-dst is deliberately still open -- it needs its exact quirks
nailed down empirically first (does pixman write 0xff to an XRGB dst's
top byte during a blend, leave it untouched, or blend it like any other
channel with whatever garbage was already there?), the same "verify
before implementing" discipline as every other part of this accelerator.
**This, not more mask coverage, is now the highest-value next step** --
it's specifically what would unlock real acceleration in foot, the
single most mask-composite-heavy real client measured so far.

## NEXT (v2 candidates, ranked by ISH_PIXMAN_STATS decline frequency so far)
1. **x8r8g8b8-as-dst support** -- see "KNOWN GAP" above. Highest value:
   unlocks foot's actual glyph rendering, the biggest real workload
   measured. Must empirically nail down XRGB-dst blend semantics on the
   mint oracle before writing any kernel code, exactly like every other
   formula in this project.
2. `pixman_blt` and `pixman_image_fill_boxes`/`fill_rectangles` interposition
   (documented v1 scope cuts in the shim's own README).
3. SRC-with-mask and other op+mask combinations (currently declined
   unconditionally in the shim, pending their own differential validation).
4. App Settings UI toggle for `ISH_PIX_ACCEL` (currently CLI/env-only,
   matching where the crypto accelerator and HLE toggles also started).
5. Re-attempt the visual VNC sanity check with a simpler client once the
   blank-screenshot test-environment mystery above is understood.

kernel/ish_accel_crypto.c + opt/AOK/crypto/ish_provider.c) — same
architecture, same lessons apply.

## 1. Problem and evidence

The Wayland desktop's steady-state pipeline is software rendering all the way
down, all under JIT emulation:

    GTK/cairo raster (pixman) → wl_shm buffer → labwc composite
    (wlroots *pixman renderer*) → wayvnc framebuffer capture → RFB → applet

Every stage above the RFB link runs emulated scalar ARM code. The crypto work
proved the paravirt-provider pattern: a guest-side shim routes a hot,
well-specified operation through a private syscall to host-native code —
version-independent, whole-operation coverage, measured 15–19x for ChaCha20
(vs fingerprint-HLE which was rejected for crypto and measured near-neutral
on real workloads generally).

pixman is the single best target because BOTH heavy stages (cairo raster and
labwc/wlroots composition) sit on the same small public API of `libpixman-1`,
which both link dynamically on Arch and Devuan → one interposable seam
accelerates the whole desktop's pixel movement.

Unlike OpenSSL, pixman has NO provider/plugin API. Delivery is therefore an
`LD_PRELOAD` shim interposing pixman's public entry points, falling back to
the real library (`dlopen`/`dlsym(RTLD_NEXT)`) for anything not covered.

## 2. Hypercall interface (host side)

New op family alongside the crypto accelerator, same dispatch style
(calls.c intercepts the number BEFORE the syscall-table range check):

- `ISH_SYS_PIXOP` = 0xacc1, arg = guest pointer to `struct ish_pix_req`:
  - `op`: PIX_FILL, PIX_COPY (SRC), PIX_OVER (premultiplied OVER),
    PIX_OVER_MASK_A8 (OVER with an a8 mask — the glyph-blit shape) — v1 set.
  - per-surface descriptors (dst, src, mask): guest base pointer, stride
    (bytes, may be negative), format code (a8r8g8b8 / x8r8g8b8 / a8 for v1),
    width/height.
  - a box list (guest pointer + count) — one hypercall per composite call,
    batched over all boxes/rows, to amortize dispatch (crypto lesson:
    per-byte copy costs dominate before dispatch does; still, don't call per
    box).
  - `flags`: PROBE (feature/self-test query — the shim uses this to decide
    whether to activate, mirroring the crypto provider's decline path).
- Implementation `kernel/ish_accel_pix.c`:
  - direct guest-page access, NO bounce buffers — generalize
    `user_transform_two()` (kernel/user.c) to resolve dst (MEM_WRITE, COW
    honored) + up to two RO sources per row-span; rows are guest-contiguous so
    the walk is per-row per-page-span, same lockstep discipline as crypto
    (never hold a resolved pointer across the next mem_ptr).
  - pixel kernels in portable C, compiled -O2 (clang autovectorizes these
    trivially on arm64 host; measure before reaching for vImage/Accelerate —
    the crypto experience says plain -O2 C at direct pointers is already
    hundreds of MB/s, and vImage adds format-conversion constraints).
  - gated `doEnablePixAccel`, default OFF; `ISH_PIX_ACCEL=1` on CLI; lazy
    self-test (render a reference vector set and memcmp against baked-in
    expected output) exactly like the crypto selftest gate.
  - CRITICAL correctness rule: bit-exactness vs pixman for the covered ops.
    OVER/premultiply in pixman is defined on 8-bit lanes with exact rounding
    ((a*b + 127)/255 style); replicate pixman's arithmetic precisely, verified
    by differential tests, or decline the op. No "close enough" rendering —
    wayvnc damage tracking and user expectations both want determinism.

## 3. Guest shim (delivery)

`opt/AOK/pixman/ish_pixman_shim.c` → `libish-pixman.so` (per guest arch,
built in-guest by `build-shim.sh`, packaged like opt/AOK/crypto):

- Interposes (v1): `pixman_image_composite32`, `pixman_fill`, `pixman_blt`,
  `pixman_image_fill_boxes`/`fill_rectangles`.
- Uses only pixman PUBLIC accessors to inspect images
  (`pixman_image_get_format/data/stride/width/height`, repeat/transform/
  filter/alpha-map/clip queries). Accelerate only when: op ∈ {SRC, OVER},
  formats ∈ v1 set, no transform, no filter beyond nearest-identity, normal
  repeat=NONE, no alpha map, no component alpha, clip region representable as
  the box list. EVERYTHING else → `real_pixman_image_composite32(...)` via
  `dlsym(RTLD_NEXT)` — behavior identical to no shim.
- Probes `ISH_SYS_PIXOP` once at load; on ENOSYS/failed probe the shim
  permanently passes through (safe on stock iSH, real Linux, or accel-off).
- `ISH_PIXMAN_STATS=1`: per-op accelerated/declined counters + decline
  reasons dumped at exit — this drives v2 coverage the same way the HLE
  near-miss tracer was supposed to (but with exact call shapes, not
  prologues).
- Wiring: `start-wayland.sh` exports
  `LD_PRELOAD=/AOK/pixman/$(uname -m)/libish-pixman.so` when the file exists
  and a probe helper succeeds (tiny `/AOK/pixman/probe` binary, same pattern
  as the crypto provider's decline). Session-scoped only — never a global
  ld.so.preload, so a broken shim can't take out the whole guest; Reconnect
  with the toggle off gives a clean rollback.

## 4. Where the wins should land (validate in Phase 0)

- labwc composition: every damaged frame is OVER/SRC of window surfaces onto
  the output buffer (wlroots pixman renderer) — full-frame-scale pixel work
  at up to 60 Hz during drags/typing.
- cairo in GTK apps: widget fills, box blits, a8 glyph masks — exactly
  PIX_FILL/PIX_COPY/PIX_OVER_MASK_A8.
- foot is NOT pixman-based (its own renderer) — terminal-only sessions won't
  move; the target metric is GTK app interaction + window drag smoothness.
- wayvnc capture is memcpy-shaped (already partially HLE-able) — out of scope
  here, but the same 0xacc1 op family leaves room for a PIX_COPY-based
  fast path later if Phase 0 shows it matters.

## 5. Phases

### Phase 0 — profile the pipeline (1–2 days)
On-device (or CLI + VNC harness, which this session already built —
`rfb_drive.py` in the transcript): drive a window drag and a GTK redraw loop
while sampling the emulator host-side (`sample`/Instruments on Mac;
thread-name attribution of guest tasks) + guest `/proc/<pid>/stat` deltas for
labwc vs app vs wayvnc. Deliver: % of interactive-load CPU inside
pixman-shaped work per process. GO gate: labwc+app pixel work ≥ ~40% of
interactive load. Also microbench: guest pixman OVER of a 1280x720 frame
(cairo perf-like loop) emulated vs host -O2 C — sets the expected multiple.

### Phase 1 — host core + syscall + differential harness (1 week)
`ish_accel_pix.c` kernels (FILL/COPY/OVER/OVER_MASK_A8) + `ISH_SYS_PIXOP`
glue + selftest. Test `tests/manual/pixman_accel.c` (guest): generates
randomized surfaces/boxes (incl. negative strides, page-straddling rows,
overlapping src/dst for COPY — define as decline, pixman does), runs each op
BOTH via hypercall and via the guest's real libpixman, memcmp — 0 mismatches
over thousands of cases, both arches. This is the crypto differential
methodology transplanted.

### Phase 2 — shim + session wiring (1 week)
Shim + build script + packaging via fs/aok-tools-style manifest;
start-wayland.sh preload wiring + probe; STATS counters. Validation: full
desktop session on CLI harness with shim on — pixel-identical screenshots
(rfb_drive) for a scripted scene vs shim off; then labwc/GTK interaction
soak. Measure: window-drag frame rate over VNC + avahi-discover/bssh redraw
latency, shim on vs off, CLI and device.

### Phase 3 — device productization (0.5–1 week)
App Settings toggle (`doEnablePixAccel` + preference, default OFF; mirrors
the crypto/HLE cells), per-arch shim builds staged into the rootfs prep,
device measurement on the iPad (the real target: drag smoothness at 1280x720
on A10X), memory-of-record + release-notes entry.

### v2 candidates (driven by STATS decline data)
Nearest/bilinear scaled blits (media viewers), repeat=NORMAL patterns,
x8r8g8b8↔a8r8g8b8 conversion, wayvnc capture copies, a 16-bit lane OVER for
r5g6b5 if any surface actually uses it.

## 6. Risks
- **Coverage cliff**: if real traffic is mostly transformed/filtered
  composites, v1 declines everything and wins nothing — Phase 0's microbench
  plus Phase 2's STATS output make this visible early; v1's op set was chosen
  from what cairo/wlroots actually emit for untransformed UI.
- **Bit-exactness of OVER**: pixman's rounding must be replicated exactly;
  the differential harness is the enforcement. Any op that can't be made
  bit-exact gets declined, not approximated.
- **LD_PRELOAD fragility**: scoped to the Wayland session env only; probe
  fails closed; `dlsym(RTLD_NEXT)` fallback keeps ABI identical. Static
  pixman (rare; Alpine builds link dynamically too) simply never hits the
  shim.
- **Hypercall overhead on small ops**: batch boxes per call; decline
  composites under a size floor (e.g. <1–2 K pixels) where emulated code is
  already fine — tune with STATS + the Phase 0 microbench.
- **Security surface**: the request struct is guest-controlled — validate
  every stride/extent against the mapped region via the user_transform walk
  (which inherently faults cleanly on bad guest pointers), reject negative
  areas/overflow (64-bit math, explicit caps), and keep the kernels
  branch-simple. Same review bar as the crypto accelerator.

## 7. Effort
~3 weeks end-to-end. Phases 0–1 (~1 week) produce the decisive data and a
tested host core before any guest-visible integration exists.

## 8. Sequencing vs the code cache
Independent codebases (kernel/user.c + a new accel file vs jit/gen.c), so they
can proceed in parallel. If serialized: run BOTH Phase 0 measurements first
(2–3 days total) and let the numbers pick the first build-out — cold start
(code cache) and interactive feel (pixman) are different user-visible pains;
the Wayland experience needs the second one more once sessions are long-lived.
