# iSH pixman-accelerator LD_PRELOAD shim

Interposes pixman's public API (`pixman_image_composite32`, `pixman_fill`,
plus the property setters needed to know an image's state -- pixman has no
getter for transform/repeat/filter/alpha-map/clip) so that plain FILL/COPY/
OVER calls on 32bpp a8r8g8b8/x8r8g8b8 surfaces run through the iSH pixman
accelerator syscall (ISH_SYS_PIXOP / kernel/ish_accel_pix.c) host-natively,
instead of pixman's own C implementation running instruction-by-instruction
under emulation. This is what cairo (GTK rasterization) and wlroots' pixman
renderer (labwc's own compositing) both sit on, so it speeds up both halves
of the Wayland desktop's steady-state rendering.

Requires the accelerator enabled (`ISH_PIX_ACCEL=1` / the app's toggle, once
wired). If unavailable, every interposed function is a pure pass-through to
real pixman -- so it is always safe to load, on stock iSH, real Linux, or
with the accelerator off.

## Measured (Phase 0, -O2 CLI build, arm64 guest, headless labwc + a GTK3
redraw-loop benchmark)
~23.5% of the interactive redraw window's wall time was inside raw pixman
calls (labwc + the app combined), consistently across repeated runs -- see
`pixman_accel_plan.md` at the repo root for the full methodology.

## Install
    sh build-shim.sh                # builds + installs to /usr/local/lib/ish-pixman
`start-wayland.sh` exports `LD_PRELOAD` automatically when it finds the
built `.so` there -- no further steps once built.

## Scope / limitations (v1)
- Only `pixman_image_composite32` (SRC/OVER, no mask) and `pixman_fill`
  (32bpp) are interposed. `pixman_blt` and `pixman_image_fill_boxes`/
  `fill_rectangles` are common v2 candidates -- not yet covered.
- No mask support (`OVER_MASK_A8`, the shape glyph/text rendering uses) --
  a real gap: cairo's font rendering is mask-heavy, so text-drawing frames
  don't accelerate yet. This is the biggest coverage item for v2 (set
  `ISH_PIXMAN_STATS=1` to see decline reasons ranked by frequency).
- Declines whenever an image has a transform, non-identity filter, non-NONE
  repeat, alpha map, or clip region set -- exactly the untransformed-blit/
  fill shape real desktop redraws mostly are, but scaled/rotated/tiled
  composites always fall back to real pixman.
- Must be built per guest arch and shipped in the rootfs (a rootfs-prep
  step, same as the crypto provider).
