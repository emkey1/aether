# iSH-AOK

iSH-AOK is a fork of [ish-app/ish](https://github.com/ish-app/ish) with local product, tooling, and platform changes for day-to-day development on this tree.

This fork is not just a rebrand. It carries fork-specific behavior, bundled roots, diagnostics work, File Provider integration, and an active experimental amd64/x86_64 guest bring-up. If you want upstream iSH, use `ish-app/ish`. If you are working in this repository, this README is the relevant one.

## What This Fork Adds

- Fork-specific app identity:
  - product name `iSH-AOK`
  - bundle root `app.ish.iSH-AOK`
- Bundled root filesystems in the app build:
  - `root.tar.gz` as `Devuan5(Debian12)` for `i386`
  - `alpine-minirootfs-3.23.3-x86.tar.gz` as `Alpine3.23.3`
  - `alpine-minirootfs-3.23.3-x86_64.tar.gz` as `Alpine3.23.3(x86_64)`
- File Provider support for exposing guest files through iOS.
- Extra diagnostics and operational changes that are specific to this fork.
- Ongoing amd64 interpreter, loader, and syscall work.

## Current amd64 Status

The amd64 work in this repo is experimental.

- The app can import and attempt to boot an `x86_64` guest root.
- The interpreter, ELF64 loader, and amd64 syscall path are under active bring-up.
- Expect early boot failures, decode gaps, and partial userland execution.
- The current development branch for that work is typically `amd64`.

Relevant files:

- [amd64_port_plan.md](amd64_port_plan.md)
- [emu/amd64_interp.c](emu/amd64_interp.c)
- [kernel/exec.c](kernel/exec.c)
- [kernel/calls.c](kernel/calls.c)

## Repository Layout

- `app/`: iOS app, UI, root selection, diagnostics, File Provider integration.
- `emu/`: guest CPU emulation, including amd64 interpreter work.
- `kernel/`: syscall translation, process model, exec, signals, memory management.
- `fs/`: filesystem layer and fakefs integration.
- `jit/`: threaded-code JIT machinery inherited from iSH.
- `tests/`: manual and automated test helpers.
- `tools/`: developer tools and host-side helpers.

## Clone

This repo uses submodules.

```bash
git clone --recurse-submodules git@github.com:emkey1/ish-AOK.git
cd ish-AOK
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

## Build Requirements

For local development you will typically want:

- Xcode
- Python 3
- Meson
- Ninja
- Clang/LLVM toolchain
- sqlite3
- libarchive

On macOS, common setup is:

```bash
brew install meson ninja llvm libarchive
```

`sqlite3` is usually already present.

## Build the iOS App

Open [iSH-AOK.xcodeproj](iSH-AOK.xcodeproj) in Xcode and build the `iSH` scheme.

Important fork-specific settings:

- Bundle IDs are driven by [app/iSH.xcconfig](app/iSH.xcconfig).
- `ROOT_BUNDLE_IDENTIFIER` defaults to `app.ish.iSH-AOK`.
- The project already uses the fork-specific debug configuration `Debug-ApplePleaseFixFB19282108`.

Command-line build:

```bash
xcodebuild \
  -project iSH-AOK.xcodeproj \
  -scheme iSH \
  -sdk iphonesimulator \
  -configuration Debug-ApplePleaseFixFB19282108 \
  build CODE_SIGNING_ALLOWED=NO
```

The iOS build scripts copy these archives into the app bundle if they are present at repo root:

- `root.tar.gz`
- `alpine-minirootfs-3.23.3-x86.tar.gz`
- `alpine-minirootfs-3.23.3-x86_64.tar.gz`

If one of those files is missing, the corresponding bundled root will not work.

## Build the Native CLI / Emulator

For emulator-side work, the Meson build is usually faster than full Xcode runs.

Initial setup:

```bash
meson setup build
```

Incremental build:

```bash
ninja -C build
```

Useful targets include:

- `build/ish`
- `build/libish.a`

For many emulator changes, this is enough:

```bash
ninja -C build libish.a
```

## Working with Root Filesystems

This fork currently exposes three bundled choices in the app:

- `Devuan5(Debian12)` for `i386`
- `Alpine3.23.3` for `i386`
- `Alpine3.23.3(x86_64)` for `amd64`

The root-selection UI and metadata handling live in:

- [app/Roots.m](app/Roots.m)
- [app/RootsTableViewController.m](app/RootsTableViewController.m)

Notes:

- `x86_64` roots are for bring-up, not for normal end-user use.
- The app records guest ABI per imported root.
- File Provider domains are synchronized for managed roots.

## Logging and Diagnostics

Logging is controlled by `ISH_LOG` in [app/iSH.xcconfig](app/iSH.xcconfig).

Examples:

```xcconfig
ISH_LOG = verbose strace
```

Current logger defaults:

- iPhone / simulator: `nslog`
- macOS: `dprintf`

Common useful channels:

- `strace`
- `verbose`
- `instr`

For emulator bring-up, the fastest loop is usually:

1. Patch interpreter or kernel code.
2. Run `ninja -C build libish.a`.
3. Rebuild the iOS app.
4. Launch in simulator.
5. Inspect console logs for faulting RIP, opcode window, and register state.

## File Provider

This fork includes an iOS File Provider extension so guest files can be surfaced through the system file APIs.

Relevant code:

- [app/FileProvider/FileProviderExtension.m](app/FileProvider/FileProviderExtension.m)
- [app/FileProvider/FileProviderEnumerator.m](app/FileProvider/FileProviderEnumerator.m)
- [app/FileProvider/FileProviderItem.m](app/FileProvider/FileProviderItem.m)

This is fork-specific functionality and should be treated as part of the maintained product surface here.

## amd64 Development Workflow

If you are working on amd64 in this repo:

- Prefer the interpreter first; do not assume the JIT path is relevant yet.
- Keep fixes small and reversible.
- Validate with both:
  - `ninja -C build libish.a`
  - simulator `xcodebuild`
- When a guest fails, capture:
  - fault type
  - guest RIP
  - opcode window
  - guest registers
  - any added targeted trace output

The current amd64 work frequently touches:

- instruction decode and execution in [emu/amd64_interp.c](emu/amd64_interp.c)
- JIT handoff and dispatch in [jit/jit.c](jit/jit.c)
- ELF64 and process startup in [kernel/exec.c](kernel/exec.c)
- syscall dispatch and ABI handling in [kernel/calls.c](kernel/calls.c)

## Branches

At the time of writing:

- `main` is the general integration branch for the fork.
- `amd64` is the active branch for x86_64 guest bring-up.

If you update cross-cutting documentation, keep both branches in sync when the change applies to both.

## Upstream Relationship

iSH-AOK is based on upstream iSH, but it is intentionally diverged.

That means:

- upstream README instructions may be incomplete or wrong for this fork
- branch names and build configurations may differ
- bundled roots and operational behavior here are fork-specific
- experimental amd64 support here should not be assumed to exist upstream

## License

See:

- [LICENSE.md](LICENSE.md)
- [LICENSE.IOS](LICENSE.IOS)
