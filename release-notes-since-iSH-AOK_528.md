# Release Notes Since `builds/iSH-AOK_528`

These notes summarize changes from `builds/iSH-AOK_527` intended for `builds/iSH-AOK_528`.

## Highlights

- Fixed the 32-bit root filesystem boot regression that broke i386 `init` very early in dynamic loader startup.
- Fixed additional 32-bit execution correctness issues that showed up after boot in SSE compare flags and double-precision shift flag handling.
- Fixed Unix domain socket peer pathname reporting for accepted local sockets.
- Hardened the build so CPU offset changes cannot silently leave stale AArch64 JIT gadget objects linked into a release binary.
- Fixed the Alpine e2e harness so fresh-root release validation provisions the right packages and actually uses the requested fakefs path.

## User-Facing Changes

### 32-Bit Runtime Stability

- Restored working 32-bit root filesystem boot on i386 userspace.
- Fixed incorrect XMM register state access caused by truncated CPU-state register offsets in the legacy i386 decoder path.
- Removed a bad AArch64 JIT TLB fast-path precheck that regressed 32-bit execution.
- Added a defensive `MAP_FAILED` check in the memory copy-on-write path.

### Instruction and Emulation Correctness

- Fixed `ucomiss` and `comiss` flag results in the vector compare path so qemu regression coverage matches native x86 behavior.
- Fixed AArch64 JIT `shld`/`shrd` overflow-flag handling for count `== 1`.
- Fixed `AF_LOCAL` accepted-socket peer pathname visibility so `getpeername()` reports the expected Unix socket path.

### Release Validation and Test Harness

- Updated e2e Alpine bootstrap to install `linux-headers` in addition to build tools and Python runtimes, allowing `net_regress` netlink coverage to compile on fresh roots.
- Fixed `tests/e2e/e2e.bash -f` so a nonexistent fakefs path is treated as a new target instead of silently falling back to `./e2e_out/testfs`.
- Verified the release path on a fresh Alpine i386 fakefs with `7/7` e2e tests passing.

## Maintainer Notes

- `builds/iSH-AOK_527` is the previous release tag.
- `builds/iSH-AOK_528` should point to commit `fbcb277c`.
- The remaining modified Xcode project file in the local worktree was intentionally left out of the release fix commit.

## Commit Range

- `fbcb277c` Fix 32-bit execution regressions and e2e coverage
