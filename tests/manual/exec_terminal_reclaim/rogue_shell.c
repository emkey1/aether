/* Stand-in for "an interactive job-control shell that seizes the
 * controlling terminal and exits without restoring it" -- the same shape
 * of bug as smallclue's `sh` (AetherOS's third_party/smallclue): on a real
 * tty it calls setpgid(0, 0) + tcsetpgrp() to take over as the terminal's
 * foreground process group, then exits without giving it back. Used by
 * verify.py to exercise vmBuiltinDosExec's terminal-reclaim fix without
 * depending on any particular real shell being installed. */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

int main(void) {
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    if (setpgid(0, 0) != 0) {
        perror("setpgid");
        return 1;
    }
    if (tcsetpgrp(STDIN_FILENO, getpgrp()) != 0) {
        perror("tcsetpgrp");
        return 1;
    }
    fprintf(stderr, "[rogue_shell] seized terminal, exiting without restoring\n");
    return 0;
}
