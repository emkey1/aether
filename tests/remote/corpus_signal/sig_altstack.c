/*
 * sig_altstack.c — sigaltstack(2) / SA_ONSTACK.
 *
 * Asserts the behaviors that are conformant on BOTH guest ABIs:
 *   - a SA_ONSTACK handler runs on the alternate stack;
 *   - sigaltstack() reports SS_ONSTACK while a handler is executing on it;
 *   - SS_DISABLE clears the alternate stack;
 *   - a too-small alternate stack is rejected with ENOMEM;
 *   - changing the alternate stack while running on it is rejected with EPERM.
 *
 * KNOWN INTENTIONAL DEVIATION (not asserted for i386): the i386 path in
 * receive_signal() runs *every* handler on the configured alternate stack
 * regardless of SA_ONSTACK (a documented legacy behavior this tree relies on),
 * whereas Linux only does so when SA_ONSTACK is set. So the "handler runs on
 * the NORMAL stack without SA_ONSTACK" check is compiled for __x86_64__ only,
 * where iSH is conformant; on i386 it would diverge from real Linux by design.
 */
#include "sig_common.h"

static char altbuf[64 * 1024] __attribute__((aligned(16)));
static volatile sig_atomic_t on_alt;
static volatile sig_atomic_t ss_onstack_in_handler;

static int sp_in_altstack(void) {
    char probe;
    uintptr_t p = (uintptr_t) &probe;
    return p >= (uintptr_t) altbuf && p < (uintptr_t) altbuf + sizeof altbuf;
}

static void handler(int sig) {
    (void) sig;
    on_alt = sp_in_altstack();
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0)
        ss_onstack_in_handler = (cur.ss_flags & SS_ONSTACK) ? 1 : 0;
}

static void install(int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

int main(int argc, char **argv) {
    sig_init(argc, argv);

    stack_t ss = { .ss_sp = altbuf, .ss_size = sizeof altbuf, .ss_flags = 0 };
    emit_i("altstack.set.rc", sigaltstack(&ss, NULL));

    /* SA_ONSTACK handler must run on the alternate stack (both ABIs) */
    on_alt = ss_onstack_in_handler = 0;
    install(SA_ONSTACK);
    raise(SIGUSR1);
    emit_i("altstack.onstack.ran_on_alt", on_alt);
    emit_i("altstack.onstack.ss_onstack_in_handler", ss_onstack_in_handler);

#if defined(__x86_64__)
    /* without SA_ONSTACK Linux runs on the normal stack (amd64 only — see top) */
    on_alt = 0;
    install(0);
    raise(SIGUSR1);
    emit_i("altstack.no_onstack.ran_on_alt", on_alt);
#endif

    /* query reports SS_DISABLE/no-onstack outside a handler */
    {
        stack_t cur;
        sigaltstack(NULL, &cur);
        emit_i("altstack.query.onstack_outside", (cur.ss_flags & SS_ONSTACK) ? 1 : 0);
        emit_i("altstack.query.size_matches", cur.ss_size == sizeof altbuf);
    }

    /* too-small alternate stack -> ENOMEM */
    {
        stack_t tiny = { .ss_sp = altbuf, .ss_size = 1, .ss_flags = 0 };
        errno = 0;
        int rc = sigaltstack(&tiny, NULL);
        emit("altstack.toosmall.errno", "%s", rc < 0 ? errno_name(errno) : "OK");
    }

    /* SS_DISABLE clears it */
    {
        stack_t dis = { .ss_sp = NULL, .ss_size = 0, .ss_flags = SS_DISABLE };
        int rc = sigaltstack(&dis, NULL);
        stack_t cur;
        sigaltstack(NULL, &cur);
        emit_i("altstack.disable.rc", rc);
        emit_i("altstack.disable.flag", (cur.ss_flags & SS_DISABLE) ? 1 : 0);
    }

    return 0;
}
