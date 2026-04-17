Focused iSH-AOK guest regression suite

These sources are exposed inside the guest at /AOK/tests.

Quick start:
  sh /AOK/tests/setup-regressions.sh --install-deps --run

If a C toolchain is already present:
  sh /AOK/tests/setup-regressions.sh --run

Verbose mode:
  sh /AOK/tests/setup-regressions.sh --run -v

Focused tests:
  atomics32.c          Combined atomic probe with single-case and stress checks
  atomic_xadd32.c      lock xaddl coverage
  atomic_cmpxchg32.c   lock cmpxchgl coverage
  atomic_cmpxchg8b.c   lock cmpxchg8b coverage
  atomic_logic32.c     lock orl/andl/xorl coverage
  signal_core.c        Core signal delivery, wait, and signalfd coverage
  signal_restart.c     SA_RESTART behavior for read and waitpid
  signal_realtime.c    sigqueue and realtime queued-signal coverage
  signal_altstack.c    sigaltstack and SA_ONSTACK coverage
  eventfd_interrupt.c  eventfd read/poll interruption via the generic wait path
  futex_core.c         FUTEX_WAIT/FUTEX_WAKE timeout, wake, and signal coverage

All focused tests accept -v or --verbose. Without it they print only failures
plus the final PASS/FAIL line for each test.
