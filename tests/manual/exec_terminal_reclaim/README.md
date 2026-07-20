# Manual verification: `exec()` terminal reclaim

Covers the fix in `vmBuiltinDosExec` (`external/pscal-core/src/backend_ast/builtin.c`),
which calls `pscalRuntimeReclaimForegroundTerminal()`
(`external/pscal-core/src/common/runtime_tty.c`) after `waitpid()` returns.

## The bug

Any Aether program that uses the core `exec()`/`dosExec()` builtin to run an
interactive, job-control-taking child (a real shell -- bash, dash, etc.) can
get stuck afterward. Such a shell calls `setpgid(0, 0)` + `tcsetpgrp()` on
startup to seize the controlling terminal for its own process group, as job
control requires. If it exits without restoring the terminal to the parent's
process group first (not all shells do -- this is the exact shape of a real
bug found in AetherOS's `third_party/smallclue`), the parent's own next
terminal read is left permanently stuck: either an immediate `EIO` (if the
parent's process group is now orphaned) or a `SIGTTIN` stop (otherwise) --
in the busy-loop case (`readkey()`'s retry-on-poll loop) this manifests as
100% CPU with no forward progress rather than a literal hang.

The fix makes `exec()` reclaim the terminal for its own process group right
after the child exits, mirroring what any job-control shell does when
regaining the foreground: temporarily ignore `SIGTTOU`/`SIGTTIN` (issuing
`tcsetpgrp()` from what is now a background process group would otherwise
trigger the exact same stop), call `tcsetpgrp(STDIN_FILENO, getpgrp())`, then
restore the previous signal disposition. It's a no-op when stdin isn't a
real TTY, and silently ignores failures (e.g. `ENOTTY`) -- `exec()` is used
in plenty of non-interactive contexts too.

## Why this isn't a `tests/*.aether` fixture

`tests/run.sh` runs the `.aether` corpus non-interactively with no
controlling terminal, so `isatty(stdin)` is false and both the bug and the
fix are no-ops there. Reproducing the actual bug needs a real pty and a
child that seizes it, which the existing harness has no support for.

## Running it

```sh
cd tests/manual/exec_terminal_reclaim
cc -o /tmp/rogue_shell rogue_shell.c
ROGUE_SHELL_PATH=/tmp/rogue_shell python3 verify.py /path/to/build/aether fixture.aether
```

Expected: `VERDICT: readkey() returned -- terminal reclaim WORKED`, exit 0.

To confirm this actually catches the bug (not just trivially passing):
revert the `pscalRuntimeReclaimForegroundTerminal()` call in
`vmBuiltinDosExec`, rebuild, and rerun -- it should instead print
`VERDICT: readkey() never returned -- BUG REPRODUCED (stuck)` and exit 1.
Confirmed both ways during development of this fix.
