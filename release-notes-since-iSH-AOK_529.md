# Release Notes Since `builds/iSH-AOK_529`

These notes summarize changes from `builds/iSH-AOK_529` intended for `builds/iSH-AOK_530`.

## Highlights

- Reworked task/session/process-group locking to remove `pids_lock` from the hottest TTY and procfs paths, fixing the multi-terminal stalls that showed up under Go builds, `top`, `watch`, and terminal churn.
- Fixed OpenSSH privilege-separation UID/GID drop behavior so `sshd` can permanently drop privileges correctly again.
- Fixed DNS refresh so guest `/etc/resolv.conf` updates when the iOS network configuration changes, including transitions between cellular, Wi-Fi, and VPN-managed resolver sets.
- Fixed socket ancillary-data handling for `ping`, including IPv4 TTL and IPv6 hop-limit metadata, and fixed blocking socket receive interruption so `^C` can break stuck `ping -4` receives again.
- Added deeper Go release-smoke and matrix tooling to validate cold builds and real-world Go projects on release candidates.

## User-Facing Changes

### Terminal, TTY, and Job Control Stability

- Removed `pids_lock` from the interactive TTY read path and narrowed job-control locking around process-group/session metadata.
- Reduced lock hold times across procfs, signal fanout, workspace process snapshots, and related task lookups.
- Fixed an `exit_group` signal-delivery lifetime bug that could crash or wedge heavy process-churn workloads.
- Improved session startup and shared-mm coordination to reduce startup and lifecycle instability under load.

### SSH and Privilege Handling

- Fixed Linux capability handling when a root-originating task permanently drops to a non-root UID/GID.
- This restores OpenSSH privsep behavior that previously caused `sshd` to abort or mis-handle permanent privilege drops.
- Added a focused regression test for the UID/GID drop invariant used by OpenSSH.

### DNS and Networking

- Switched DNS refresh to use live Apple DNS configuration data instead of relying only on stale resolver state.
- DNS updates are now triggered from DNS-configuration notifications and path changes, then written back into guest `/etc/resolv.conf`.
- Aggregated usable resolver entries more defensively so stale Wi-Fi resolvers are less likely to persist after path changes.
- Fixed translation of host ancillary socket metadata so guest IPv4 `ping` sees TTL correctly and guest IPv6 `ping` sees hop-limit metadata correctly.
- Hardened host control-message parsing during network churn and made blocking `recvmsg()` / `recvfrom()` paths interruptible via the existing signal-unwind path.

### Runtime Compatibility and Correctness

- Preserved NaNs correctly in SSE float widening conversions.
- Improved shared-mm quiescing and host runtime stability.

### Test and Release Tooling

- Added a deeper synthetic Go release-smoke harness.
- Added a repeatable Go release matrix driver for synthetic smoke plus real-world external Go repos.
- Added new net-regression tests for ancillary metadata delivery and `recvmsg()` interruption.

## Known Issue

- IPv6 still has a path-transition weakness: in current testing, IPv4 survived some Wi-Fi/cellular primary-path flips while IPv6 could still die across the same transition. This is noted but not fixed in `builds/iSH-AOK_530`.

## Maintainer Notes

- `builds/iSH-AOK_529` points to commit `efd1f90d`.
- `builds/iSH-AOK_530` should point to the current release candidate commit after this note is committed and tagged.
- Release validation status at tag time:
  - synthetic Go smoke passed on the current baseline
  - external Go matrix automation was corrected to use build-focused verification rather than `-h`/`--help` semantics

## Commit Range

- `81fa8a82` fastlane: retarget release config to iSH-AOK
- `c06f962c` docs: correct iSH-AOK 529 release notes
- `0c86eace` runtime: improve shared-mm quiescing and host stability
- `074372c7` emu: preserve NaNs in SSE float widening conversions
- `5d03bd43` stabilize session startup and shared-mm coordination
- `ca2eb694` split tty session metadata from pid topology
- `a59c1613` fix exit-group signal delivery lifetime
- `7864511a` tighten task snapshot and group-state locking
- `969c8c7a` reduce pid-lock hold times in signal and task lookups
- `7707f084` fix uid capability drop for sshd privsep
- `e4a5d424` fix network transition handling and add release matrix
