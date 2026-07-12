# Release Notes Since `builds/iSH-AOK_538`

These notes summarize changes from `builds/iSH-AOK_538` intended for `builds/iSH-AOK_539`.

The headline is a new **Workspace file manager and viewer suite**: a Finder-style File Manager applet plus Markdown, Image, and Video viewer applets, all built on a shared guest-filesystem bridge, landed over a sequence of commits that also fixed real crashes and hangs an adversarial review caught along the way (open-on-launch crashes, a FIFO-open deadlock, an unbounded recursive-delete stack overflow, a persist-path escape) and a text-contrast bug found only after the first device pass. Also in this range: two 64-bit-guest correctness fixes (amd64 `preadv2`/`pwritev2`, ABI-aware crash dumps), a riscv64 `rdtime`/`rdcycle`/`rdinstret` fix needed by Go binaries, a new downloadable Devuan 6 riscv64 root, and a fix for a false `EADDRINUSE` that could make a service (e.g. `sshd`) lose its listening socket on restart while any client was still connected to it.

## Highlights

- **Workspace: File Manager, Markdown Reader, Image Viewer, and Video Player applets.** A Finder-style file manager (sidebar, breadcrumb navigation, sort/hidden-files/new-folder, row context menu, swipe-to-delete, hardware-keyboard shortcuts) opens files by extension into a non-editable Markdown reader (with pipe-table rendering), a pinch-zoomable Image Viewer, or an AVKit-backed Video Player, all sharing a new `GuestFileBridge` that factors the "borrow pid 1, call `generic_open`" pattern previously duplicated across the app. All four are wired into the Modern Launcher's "Add Built-in" list and persist their open file/directory across app restarts.
- **A real bind-conflict bug fixed: restarting a listening service could silently lose one address family.** An Apple-only preemptive `EADDRINUSE` check (added to guard a real host quirk where every guest task shares one host process) didn't distinguish a genuine listening socket from a plain established connection — so any live connection to a port (e.g. the very SSH session used to restart `sshd`) could make a fresh `bind()` on that port falsely fail, while a different address family with no live connection bound fine. Symptom looked exactly like a partial/flaky restart with no error surfaced anywhere. Now only actual listeners count as a conflict.
- **riscv64: `rdtime`/`rdcycle`/`rdinstret` CSR reads implemented.** Go binaries call `runtime.nanotime()` via `csrrs rd, time, x0`, which the riscv64 CSR decoder didn't recognize (it only handled the FP CSRs) — any Go binary SIGILL'd on its first clock read. All three read-only Zicntr counters now alias a host monotonic-clock reader.
- **amd64 `preadv2`/`pwritev2` (syscalls 327/328) implemented** — previously missing table entries caused a hard "missing syscall" crash (hit by `gpg` in normal use, misreported in an earlier log as `eventfd2`, which was already correctly wired).
- **Crash dumps are now ABI-aware.** `dump_stack()`/`dump_mem()` unconditionally read the i386-only `cpu.esp`/`ebp`/`eip` fields, so a page-fault crash dump on an amd64/arm64/riscv64 task printed garbage SP/FP/PC and cut short with a bogus memory-read failure. Now selects the right registers per guest ABI.
- **App**: a downloadable Devuan 6 riscv64 root, and an opt-in second fakefs mount (`ISH_FAKE_MNT2`) for reproducing cross-root bugs (e.g. `mv` between two `/AOK/roots`-style mounts) from the CLI harness without the iOS app.

## User-Facing Changes

### Workspace: file manager and viewers (new)

- `GuestFileBridge` — an async, cancelable bridge factoring the "borrow pid 1, call `generic_open`" guest-VFS pattern out of `AudioLibrary.m`/`MotePadDocumentStore.m` into one shared component, plus a `WorkspaceFileOpenable` routing protocol so new applets can live in their own files instead of growing the 12.9k-line `WorkspaceViewController.m` (`24af419d`).
- **File Manager**: sidebar of fixed locations, back/forward/up navigation, sort/hidden-files/new-folder via a pull-down menu, row context menu (Open/Rename/Duplicate/Get Info/Delete), swipe-to-delete, a Finder-style tappable breadcrumb path, a status bar showing item count and free space (`filesystemStatusAtGuestPath:`, mirroring `statfs`), and hardware-keyboard shortcuts (cmd-up, cmd-[/], cmd-shift-., cmd-R, cmd-N) (`24af419d`, `64e4d804`).
- **Markdown Reader**: renders through the LLM chat's shared `MarkdownRenderer` (now including pipe-table support, rendered as monospaced grids) in a non-editable view, follows relative document links in place with back history, opens `http(s)` links externally; capped at 4 MiB (`7628fb4d`, `64e4d804`).
- **Image Viewer**: pinch/double-tap zoomable, decoded through ImageIO with a bounded pixel budget so a huge photo can't balloon into a full-resolution `UIImage` and trigger jetsam; fit/actual-size toggle, prev/next navigation across sibling images, share sheet; capped at 64 MiB (`7628fb4d`).
- **Video Player**: first use of AVKit in the app — realfs files play directly from their host URL, fakefs files extract to a temp file first with a cancelable progress overlay; hands any URL to `AVPlayerItem` so an unsupported container fails with AVFoundation's real error instead of a guessed one. Audio/video coexistence needed no new code, since `AudioPlayerEngine` already pauses itself on session interruption (`c5607160`).
- Music applet now conforms to the same file-open routing, so opening an audio file from the File Manager replaces the queue and plays it; the File Manager's directory context menu gained "Add Folder to Music" (`c5607160`).
- All four applets are reachable from the Modern Launcher's "Add Built-in" list, which had its own separate hardcoded list that was missed when the applets were wired in (`e8beaa80`), and persist their open file/directory across app restarts via a new `WorkspaceStatefulTool` protocol (the Video Player deliberately opts out — auto-playing a large video on every launch would be hostile) (`226de719`).
- **Adversarial review pass fixed real bugs found in the four commits above** (`ab7f4506`):
  - Crashes: all three new applets built theme-color arrays from still-nil button ivars in `viewDidLoad`, before their views existed — every window crashed on open. Guarded like the Music applet already did.
  - `GuestFileBridge` extraction opened guest files without `O_NONBLOCK`, so opening a FIFO (which blocks awaiting a writer) wedged the bridge's serial queue forever, hanging every bridge operation app-wide; now non-blocking and rejects non-regular files.
  - The extraction cache was an `NSCache` whose eviction deleted backing temp files out from under a playing video on memory pressure; now a plain dictionary reclaimed only by the app-launch sweep.
  - Recursive delete used one stack frame per directory level, overflowing a 512KB worker stack on deep guest-constructable trees; converted to an iterative post-order walk.
  - A crafted guest path containing `..`, or a `/AOK/persist` symlink resolving outside the persist area, could escape the persist directory mapping into the app container; both are now blocked.
  - Image Viewer arrow keys never fired (iOS 15+'s focus engine consumes unmodified arrows); Markdown reader now strips `#fragments` and percent-decodes relative links.
- **Text contrast fixed** (`59e7d2b8`): the theme system only guarantees its 7.0/4.9/4.8 contrast ratios against the opaque `card`/`cardAlt` surface colors, never against the decorative gradient backdrop every window sits on. The four new applets were the first to put text directly on that transparent gradient, which (worst in the Aurora theme) could fade rows to near-invisible depending on the active theme. All four now back their content view with the same opaque card tint every other applet uses.

### JIT / CPU emulation

- **riscv64**: `rdtime`/`rdcycle`/`rdinstret` (the read-only Zicntr CSRs) now alias a host monotonic-clock read instead of hitting the undefined-instruction path — Go binaries (e.g. `glow`) SIGILL'd immediately on their first clock read via `runtime.nanotime()`'s `csrrs rd, time, x0` (`5d2e185d`).
- **amd64**: `preadv2`/`pwritev2` (327/328) implemented, delegating to the existing `preadv`/`pwritev` cores through the 64-bit-safe native syscall path; an offset of `-1` uses-and-advances the current file position per the syscall's semantics, `RWF_*` flags are accepted but ignored (same tradeoff as the existing `fadvise64` stub). Verified against a real x86_64 binary round-tripping data through both syscalls via the mint Lima VM oracle (`7226a0d8`).
- **Crash diagnostics**: `dump_stack()`/`dump_mem()` were i386-only, reading `cpu.esp`/`.ebp`/`.eip` unconditionally regardless of guest ABI — a page-fault dump on amd64/arm64/riscv64 printed garbage SP/FP/PC and cut short with a bogus "Unable to read memory" instead of a real stack dump. Now ABI-aware and 64-bit-address-safe; no change to fault handling or signal delivery itself (`91cdbec5`).

### Networking

- Fixed a false `EADDRINUSE` on `bind()`: an Apple-only preemptive conflict check (added to compensate for every guest task sharing one host process) treated any socket sharing a domain/type and overlapping local address as a conflict, without checking whether it was actually listening — since an accepted connection's local address is the same port as its listener, a live connection to a port could make a fresh `bind()` there fail even though nothing was really occupying the slot. Reproduced via `sshd`: restarting it while an SSH session stayed connected lost the IPv4 listener (the live connection was IPv4) while IPv6 rebound fine (no live IPv6 connection to collide with) — `sftp`/`scp` then failed with a plain connection refusal and nothing in any log, since the failure never reached sshd's own error path. Fixed to only treat genuine listeners as conflicts (`6894e0ce`).

### App / roots

- A downloadable Devuan 6 riscv64 root added to the in-app filesystem chooser (`a211e82a`).
- CLI harness (`main.c`) gains an opt-in second fakefs mount at `/fakemnt2` (`ISH_FAKE_MNT2`) for reproducing cross-root bugs — e.g. `mv` between two `/AOK/roots`-style fakefs mounts — without needing the iOS app (`a211e82a`).

## Known Issues

- The futex `SA_RESTART` lost-wake issue remains open (deferred; real-software-immune, device-only repro; an earlier attempted fix was reverted).
- `NETLINK_AUDIT` remains unimplemented ("Failed to connect to audit daemon" from anything that probes it).
- `--sockabuse` can still hang on a second, less-understood `recvfrom` wedge distinct from the signal-mask bug fixed in an earlier release.
- Two build/devuan fakefs images already built with the old `fakefsify` (`build/devuan-x86`, `build/devuan-x86_64`) still carry a bad `dev_t` encoding fixed for future imports only; need regenerating or in-guest `mknod` repair.
- AArch64 and riscv64 remain the newest guest engines and are still being hardened; expect rougher edges than the established i386/amd64 engines.
- The new Workspace file-manager/viewer suite has not yet had thumbnails, drag-and-drop, multi-select, `chmod`, GIF support, or streaming video added — tracked as follow-up work, not regressions.

## Maintainer Notes

- `CURRENT_PROJECT_VERSION` bumped to 539 across the four main-target build configs in `iSH-AOK.xcodeproj/project.pbxproj`; the secondary (autocomplete-dummy) target's four configs remain frozen at 529, per existing convention.
- The bind-conflict fix (`6894e0ce`) was found and root-caused this cycle while investigating a live `sftp`/`scp` bug report on the M4 iPad rig (device: real network, Alpine riscv64 guest) — the original report ("ssh works, sftp/scp immediately close, nothing in dmesg") turned out to be two independent issues: a guest `sshd_config` path mismatch (Alpine's `sftp-server` lives at `/usr/lib/ssh/sftp-server`, not the Debian-style `/usr/lib/openssh/sftp-server` the config referenced) plus this emulator-level bind-conflict bug, which only surfaced once the config fix's own service restart was tested from an active SSH session. No dedicated regression test added yet for the bind-conflict fix; worth a follow-up `fs/sock.c`-level test that binds/listens, accepts a connection, then rebinds the same address from a second listener while the first connection stays open.
- CLI build verified clean (`ninja -C build ish`) after the bind-conflict fix; not yet run through the on-device `/AOK/tests` suite or reached the device (app rebuild required).
- Workspace applet work (`24af419d` through `59e7d2b8`) was developed and reviewed in-session, including a dedicated adversarial review pass (`ab7f4506`) before the persistence/gap-closing/launcher-registration follow-ups; simulator-validated for the text-contrast fix, not yet device-tested as a whole suite.

## Commit Range
```
141beb45 build: bump project version to 539
6894e0ce fs/sock: don't count established connections as bind conflicts
59e7d2b8 workspace: fix text contrast in the four new applets
e8beaa80 workspace: add the new applets to the Launcher's "Add Built-in" list
64e4d804 workspace: close the phase-4 plan gaps (breadcrumb, free space, shortcuts, md tables)
226de719 workspace: persist applet content state across app restarts
ab7f4506 workspace: fix review findings in the file-manager/viewer applets and GuestFileBridge
c5607160 workspace: add Video Player applet and wire Music into file-open routing
7628fb4d workspace: add Markdown reader and Image Viewer applets
24af419d workspace: add GuestFileBridge, shared MarkdownRenderer, and a Finder-style File Manager applet
5d2e185d kernel: implement riscv64 cycle/time/instret CSR reads
a211e82a roots: add Devuan 6 riscv64 minirootfs entry; add ISH_FAKE_MNT2 dev harness mount
960ba6e1 docs: fold the amd64 preadv2/pwritev2 fix and dump_stack ABI fix into the release notes
7226a0d8 kernel: implement amd64 preadv2/pwritev2 (syscalls 327/328)
91cdbec5 kernel: make dump_stack/dump_mem ABI-aware for 64-bit guests
```
