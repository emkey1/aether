#!/usr/bin/env python3
"""Manual regression check for the exec()/dosExec terminal-reclaim fix
(vmBuiltinDosExec in external/pscal-core/src/backend_ast/builtin.c calling
pscalRuntimeReclaimForegroundTerminal() from external/pscal-core/src/common/
runtime_tty.c after waitpid()).

Not wired into ctest: this repo's test corpus (tests/run.sh) runs the
.aether fixtures non-interactively, with no controlling terminal, so the
job-control bug this guards against can't manifest there at all (isatty()
on stdin is false, so the reclaim -- and the bug it fixes -- are both
no-ops). Exercising the actual bug needs a real pty and a foreground child
that seizes it, hence this standalone script instead of a tests/*.aether
pass/fail fixture.

What it does: builds rogue_shell.c (a minimal stand-in for an interactive
job-control shell that seizes the controlling terminal via setpgid+tcsetpgrp
and exits without restoring it -- the same bug shape as smallclue's `sh`),
runs fixture.aether under a real pty via the aether binary you point it at,
and confirms the interpreter's own readkey() call still works afterward.

Usage:
    cc -o /tmp/rogue_shell rogue_shell.c
    ROGUE_SHELL_PATH=/tmp/rogue_shell python3 verify.py /path/to/aether/build/aether fixture.aether

Expected result: "VERDICT: readkey() returned -- terminal reclaim WORKED",
exit code 0. To confirm this script actually detects the bug (rather than
trivially passing), temporarily revert the fix, rebuild, and rerun -- it
should instead print "VERDICT: readkey() never returned -- BUG REPRODUCED
(stuck)" and exit 1.
"""
import os
import pty
import select
import sys
import time

AETHER_BIN = sys.argv[1]
SCRIPT = sys.argv[2]

if "ROGUE_SHELL_PATH" not in os.environ:
    sys.exit("set ROGUE_SHELL_PATH to a built rogue_shell binary (cc -o rogue_shell rogue_shell.c)")

pid, master_fd = pty.fork()
if pid == 0:
    os.execv(AETHER_BIN, [AETHER_BIN, SCRIPT])
    os._exit(127)

output = b""
deadline = time.time() + 8
sent_key = False
saw_before = False
saw_after = False

while time.time() < deadline:
    r, _, _ = select.select([master_fd], [], [], 0.2)
    if master_fd in r:
        try:
            chunk = os.read(master_fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        output += chunk
        sys.stderr.write(chunk.decode(errors="replace"))
        sys.stderr.flush()

    if b"before-readkey" in output:
        saw_before = True
        if not sent_key:
            time.sleep(0.2)
            try:
                os.write(master_fd, b"x")
            except OSError:
                pass
            sent_key = True

    if b"after-readkey" in output:
        saw_after = True
        break

time.sleep(0.1)
try:
    wpid, status = os.waitpid(pid, os.WNOHANG | os.WUNTRACED)
except ChildProcessError:
    wpid, status = 0, 0
stopped = bool(wpid == pid and wpid and os.WIFSTOPPED(status))

print("---RESULT---")
print("saw_before_readkey:", saw_before)
print("saw_after_readkey:", saw_after)
print("child_reported_stopped:", stopped)

if saw_after:
    print("VERDICT: readkey() returned -- terminal reclaim WORKED")
    sys.exit(0)
else:
    print("VERDICT: readkey() never returned -- BUG REPRODUCED (stuck)")
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass
    sys.exit(1)
