# aarch64 Guest Port — Credits

The aarch64 (native ARM64 guest) work on this branch is motivated by, and in
places directly adapted from, [OpenMinis/ish-arm64](https://github.com/OpenMinis/ish-arm64),
the fork of `ish-app/ish` powering the Open Minis app's "Linux shell" feature.
Both `ish-AOK` and `ish-arm64` are GPLv3 derivatives of the same upstream
(`ish-app/ish`), so attribution here is required by license and is also just
the right thing to do — they did real engineering work identifying that
same-architecture (arm64 guest on arm64 host) dispatch avoids the
cross-architecture translation cost that dominates i386/amd64 emulation.

This file tracks, at the file level, which parts of this port are adapted
from their source versus independently written against iSH-AOK's own
conventions. Every commit that adapts their code names the source file(s) in
its commit message in addition to the entry added here.

See [aarch64_guest_plan.md](aarch64_guest_plan.md) for the overall plan this
credits file supports.

## Adapted / closely modeled on OpenMinis/ish-arm64

_(entries added as patches land — none yet; patch 1 is scaffolding-only and
introduces no adapted code)_

## Independently written, informed by their design

- `kernel/abi/arm64.h`, the `GUEST_ABI_ARM64` enum value, and the
  `guest_abi_desc`/`guest_abi_vm_layout` cases in `kernel/abi.h` — written
  against iSH-AOK's existing `guest_abi` dispatch machinery (which
  OpenMinis' fork does not have; their arm64 support is a compile-time
  `#ifdef GUEST_ARM64` build variant, not a per-task runtime ABI). The
  48-bit VA / top-of-address-space stack placement choice mirrors their
  rationale (avoiding a V8 CodeRange collision) but the mechanism differs
  because the surrounding infrastructure differs.

## Syscall numbering reference

Linux aarch64 syscall numbers (asm-generic based) are ABI facts, not
copyrightable expression — using the standard numbering is not "adapted
from" anyone. OpenMinis' `kernel/arch/arm64/calls.c` is still worth
consulting as a reference for table shape and stub choices when patch 4
(syscall table) lands, and will be credited here if any structure beyond the
numbers themselves is reused.
