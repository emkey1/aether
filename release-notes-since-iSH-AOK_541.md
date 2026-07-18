# Release Notes Since `builds/iSH-AOK_541`

A shorter cycle, headlined by root-causing and fixing a "gdb is totally unusable" report all the way through: three real emulator bugs (a missing exec permission flag, a 64-bit address truncation in `/proc/pid/mem`, and arm64's JIT having no single-step support at all) plus two more on amd64 (a missing `int3` instruction and a debug-register read gap) that, combined, made software breakpoints silently never trigger on both arm64 and amd64 guests. Also in this range: a real Dependabot security fix, two Arch Linux minirootfs images repaired after a previous pruning pass accidentally broke `/sbin/init`, a batch of Arch-provisioning reliability fixes (pacman sandboxing/signature/DNS-race issues), a `/proc/sys/kernel/random` gap that spammed every new Arch shell with an error, and small Workspace/ktop UX additions.

## Highlights

- **Root-caused and fixed "gdb debugging is totally unusable" end-to-end on arm64, and substantially on amd64.** The investigation turned up five distinct bugs, none of them about filesystem type (fakefs vs realfs, the original report's guess) — all guest-architecture-specific:
  - `P_EXEC` was never set on any ELF-loaded segment (`kernel/exec.c` checked the ELF program header's writable bit but never its executable bit), so every process's own code showed as non-executable in `/proc/pid/maps`. Harmless to actual execution (the fault path doesn't enforce it) but misleading to any tool, like gdb, that reads the reported permissions.
  - The real root cause of "breakpoints in a PIE binary silently never trigger": `/proc/<pid>/mem` truncated the (genuinely 64-bit) file offset to a 32-bit type before translating it. gdb prefers writing breakpoints through `/proc/pid/mem`; since PIE binaries load at high (>4GB) addresses, every breakpoint write landed at the wrong (truncated) address — but read back "correctly" from that same wrong address, so gdb never saw an error. The debuggee just silently ran to completion every time. Non-PIE binaries happened to link low enough to dodge this by pure coincidence, which is exactly why the bug looked like an all-or-nothing "gdb is broken" problem rather than a narrow one.
  - Once that was fixed, breakpoints correctly trapped — but a debugger's *own* internal "restore the original instruction and step over the breakpoint" sequence (which every debugger does, even just to cleanly report the very first hit) silently ran the entire rest of the program instead of one instruction. Root cause: arm64's JIT dispatch never checked the single-step flag at all — amd64's JIT gets single-step almost for free by falling back to its plain interpreter, but arm64 has no interpreter fallback (JIT-gadget-only), so nobody had implemented single-step there. New `cpu_single_step_arm64` compiles and executes exactly one guest instruction as a private, throwaway JIT block.
  - Separately on amd64: `int3` (`0xcc`, the actual x86 software breakpoint instruction, and what gdb's own startup self-test uses to probe the host) was entirely unimplemented in the amd64 interpreter, raising `SIGILL` instead of `SIGTRAP` — this alone broke every amd64 gdb breakpoint, not just the self-test. Fixed alongside a related gap where reading the (nonexistent, since iSH has no hardware breakpoint support) x86 debug registers returned an I/O error instead of a truthful zero.
  - Verified end-to-end against real gdb on arm64: `break`/`run`/`print`/`next`/`step`/`continue` on a PIE binary now produces output identical to real Linux, plus 600+ consecutive single-instruction steps through musl's dynamic linker startup with zero crashes. On amd64, breakpoints now correctly trap with proper arguments and backtraces; a further, narrower "`next`/`step` right after a breakpoint hit" issue was found but not root-caused, and is tracked separately (issue #503) rather than left silently unmentioned.
- **A real Dependabot security fix**: bumped `fastlane` to 2.237.0, which resolves `excon` to 1.6.0, fixing a moderate-severity header-redaction gap in redirect handling (GHSA-48rx-c7pg-q66r / CVE-2026-54171). `fastlane` 2.235.0 directly pinned an `excon` ceiling that blocked any fix; 2.237.0 relaxed it, so bumping fastlane alone was sufficient — a minimal, clean dependency-lock diff.
- **Two Arch Linux minirootfs images were quietly broken since they were first added last cycle**: the pruning pass that shrank their download size removed `usr/lib/systemd` entirely (reasoning "iSH doesn't run systemd as PID 1, so its files are unused") — but `/sbin/init` is a symlink *into* that directory, so deleting it took the actual `systemd` binary with it, turning `/sbin/init` into a dangling symlink on both images. iSH's boot logic checks that the configured boot command actually exists before running it; a dangling `/sbin/init` silently fell back to a degraded single-shared-console boot path instead of normal per-window terminals — which is what actually surfaced live as "the Workspace Terminal launcher's list stays empty" and various `ttyname()`-dependent tool failures. Both images re-extracted and re-uploaded with `usr/lib/systemd` restored.

## User-Facing Changes

### Debugging (gdb)

- `kernel/exec.c`: `P_EXEC` now set on executable ELF segments, so `/proc/pid/maps` correctly reports `r-xp` instead of `r--p` for code (`d9701556`).
- `fs/proc/pid.c`: fixed `/proc/<pid>/mem` truncating 64-bit addresses to 32 bits — the actual root cause of PIE-binary breakpoints silently never triggering (`5ac55d6b`).
- `jit/jit.c`: implemented arm64 `PTRACE_SINGLESTEP` (new `cpu_single_step_arm64`), previously a complete no-op that behaved like `PTRACE_CONT`. Fixes issue #501 (`63006ee7`).
- `emu/amd64_interp.c`: implemented `int3` (`0xcc`), previously entirely missing and raising `SIGILL` instead of `SIGTRAP` — broke every amd64 gdb breakpoint (`46f64a02`).
- `kernel/ptrace.c`: `PTRACE_PEEKUSER` on amd64's debug-register range now returns 0 instead of `EIO`, matching "no hardware breakpoints ever armed" instead of erroring (`83c53bd6`).

### Security

- Bumped `fastlane` to 2.237.0 / `excon` to 1.6.0, fixing GitHub Dependabot alert #18 (GHSA-48rx-c7pg-q66r / CVE-2026-54171) (`4e609d9f`).

### Roots

- Both Arch Linux minirootfs images (x86_64, aarch64) repaired: `usr/lib/systemd` restored after a prior pruning pass left `/sbin/init` a dangling symlink, silently degrading boot to a shared-console fallback (`cee8a3a6`).

### Filesystem / procfs

- `/proc/sys/kernel/random/{uuid,boot_id}` implemented — `bash` >= 5.1 and various distro rc scripts read these unconditionally for `$RANDOM` seeding; missing entirely before, spamming every new Arch shell with a "No such file or directory" error on stderr (`26738bd2`).

### Arch Linux provisioning reliability

- `provision-ultimate-archlinux.sh` and friends: interactive hostname prompt added, matching the existing timezone/user prompts (`7b7b099f`); pacman's Landlock sandbox disabled (unsupported by iSH's kernel) (`7b7b099f`); pacman signature verification disabled (a fresh Arch keyring can't populate under iSH's fragile gpg-agent support; HTTPS-only mirrors remain the transport) (`6ec8af98`); waits for `/etc/resolv.conf` and retries `pacman -Sy` once on transient DNS failure during early boot (`1f85b0e9`); creates the `/dev/fd`/`/dev/std{in,out,err}` symlinks Arch normally provisions via systemd-tmpfiles, which iSH never runs (`5b66db66`); force-reinstalls `glibc`/`linux-api-headers` to self-heal a prior pruning pass that had stripped `/usr/include` entirely from the aarch64 image (`15cfcc25`); generates missing sshd host keys at service-start time, since Arch normally generates them via a systemd unit iSH never runs (`f3d40b8b`).

### Workspace / tools

- Added an "Auto-Show Keyboard" toggle to the Workspace menu — terminals previously always grabbed the keyboard on load/focus/tab-switch whether wanted or not; now optional (default unchanged), with direct taps still focusing manually (`b01586de`).
- `ktop`: `1` now collapses the per-cpu meters into a single combined "Avg" summary, mirroring real `top`'s toggle (reversed, since ktop defaults to per-cpu) — useful on many-core hosts or narrow terminals (`1bcc3259`).

## Known Issues

- **Issue #503** (amd64 gdb `next`/`step` crashes with `SIGILL` immediately after a breakpoint hit): found while verifying the amd64 gdb fixes above, reproduced on both PIE and static binaries (rules out any PIE-relocation involvement), but not root-caused. A hand-rolled ptrace reproducer mimicking gdb's own breakpoint-restore-and-singlestep sequence works correctly, so the gap is specifically between that and whatever gdb's actual internal sequence does differently.
- Arch Linux support remains explicitly experimental: real util-linux `login` + PAM with none of this project's distro-specific patching, and no systemd PID 1 — this cycle's fixes make the *rootfs* boot correctly and provisioning more reliable, but don't change that systemd itself likely can't run as PID 1 under iSH (cgroups/netlink/mount-namespace gaps, not investigated).

## Maintainer Notes

- **Ran the full guest regression suite across all four architectures concurrently** on the local CLI harness (i386/`alpinex86`, amd64/`alpine64`, arm64/`alpine-arm64-test`, riscv64/`alpine-riscv64-test`), matching the project's standard concurrent multi-arch release-testing procedure. Found and killed a genuinely stale, day-old hung process left over from earlier in this session before starting, to avoid a repeat of a previously-documented contamination trap.
- `CURRENT_PROJECT_VERSION` bumped to 542 across the four main-target build configs in `iSH-AOK.xcodeproj/project.pbxproj`; the secondary (autocomplete-dummy) target's four configs remain frozen at 529, per existing convention.
- The gdb investigation's methodology is worth calling out for future hard-to-diagnose emulator bugs: rather than trying to read gdb's own C++ internals, a minimal hand-rolled `PTRACE_TRACEME` + `execve` C reproducer (mirroring exactly what a debugger does at the ptrace level) isolated "is this a gdb bug or an iSH bug" at each layer far faster, and a git-stash A/B directly confirmed the debug-register fix was not the cause of the separate #503 issue rather than leaving that as a guess.
- On-device `/AOK/tests` run on all installed roots and app rebuild/reach-device validation of this cycle's Arch/Workspace fixes remain the maintainer's tag-time step, as usual — this session's testing was all CLI-side.

## Commit Range
```
83c53bd6 kernel/ptrace: PEEKUSER on amd64 debug-register range returns 0, not EIO
46f64a02 emu/amd64: implement int3 (0xcc), the standard x86 software breakpoint
63006ee7 jit/arm64: implement PTRACE_SINGLESTEP (#501)
5ac55d6b fs/proc/pid: fix /proc/<pid>/mem truncating 64-bit addresses to 32 bits
d9701556 kernel/exec: set P_EXEC on PT_LOAD segments marked executable
f3d40b8b tools: generate missing sshd host keys in start-aok-services
1bcc3259 ktop: '1' collapses the per-cpu meters into one overall-load summary
15cfcc25 tools: force-reinstall glibc/linux-api-headers in the ultimate Arch provisioner
5b66db66 tools: create /dev/fd symlinks in the ultimate Arch provisioner
6ec8af98 tools: disable pacman signature verification (gpg keyring unusable under iSH)
b01586de app: add Auto-Show Keyboard toggle to the Workspace menu
1f85b0e9 tools: wait for resolv.conf and retry pacman -Sy on transient DNS failure
7b7b099f tools: prompt for hostname in ultimate provisioners; disable pacman Landlock sandbox
26738bd2 procfs: implement /proc/sys/kernel/random/{uuid,boot_id}
cee8a3a6 Roots: fix both Arch minirootfs images -- restore usr/lib/systemd
4e609d9f deps: bump fastlane to 2.237.0, resolving excon to 1.6.0
```
