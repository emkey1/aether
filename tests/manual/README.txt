Focused iSH-AOK guest regression suite

These sources are exposed inside the guest at /AOK/tests.

Quick start:
  sh /AOK/tests/setup-regressions.sh --install-deps --run

If a C toolchain is already present:
  sh /AOK/tests/setup-regressions.sh --run

Verbose mode:
  sh /AOK/tests/setup-regressions.sh --run -v

Simple amd64 JIT timing benchmark inside the guest:
  sh /AOK/tests/x86/amd64_jit_bench.sh
  For short commands, use -n to amplify timing differences, e.g.:
  sh /AOK/tests/x86/amd64_jit_bench.sh -n 5

Host-side amd64 GAS encoding probe:
  tests/manual/x86/amd64_gas_probe.sh -r /path/to/amd64-root-with-binutils
  This compares suspect GNU as mnemonic encodings against paired .byte
  encodings and falls back to one-file-per-case when as aborts early.

Layout:
  *.c                  Portable tests, built and run on every guest arch.
  x86/                 x86-only tests (lock-prefixed inline asm, amd64 JIT
                       benchmarks/probes). Built on i386/x86_64 guests.
  arm64/               AArch64-only tests. Built on aarch64 guests.

Focused tests (x86/, i386 + x86_64 guests):
  atomics32.c          Combined atomic probe with single-case and stress checks
  atomic_xadd32.c      lock xaddl coverage
  atomic_cmpxchg32.c   lock cmpxchgl coverage
  atomic_cmpxchg8b.c   lock cmpxchg8b coverage
  atomic_logic32.c     lock orl/andl/xorl coverage

Focused tests (arm64/, aarch64 guests):
  atomics64.c          LDXR/STXR + LDAXR/STLXR exclusives (8-64 bit), CLREX,
                       LDAR/STLR, __atomic builtins, HWCAP-gated LSE, and a
                       multithreaded stress mix (fetch_add/or, cmpxchg retry,
                       acquire/release message passing)
  arm64_regress.c      One check per real arm64-JIT bug class: ldp32 upper-half
                       zeroing, EOR/BSL vector decode, >1-page straight-line
                       blocks, 48-bit TLB aliasing, high-pointer syscall args,
                       raw-brk heap growth, MRS (CNTVCT/CNTFRQ/NZCV/ID regs),
                       CRC32 known-answer, FRECPE/CMxx-zero scalars
  vector_smoke.c       NEON intrinsics vs volatile scalar reference loops:
                       three-same int, saturating, pairwise, across-lanes,
                       widening/narrowing, shifts, permute (zip/uzp/ext/tbl),
                       two-reg misc, FP arithmetic/compares/converts/FMA

Portable focused tests (all guest arches):
  signal_core.c        Core signal delivery, wait, and signalfd coverage
  signal_restart.c     SA_RESTART behavior for read and waitpid
  signal_realtime.c    sigqueue and realtime queued-signal coverage
  signal_altstack.c    sigaltstack and SA_ONSTACK coverage
  signal_poll.c        poll/select/pselect signal interruption coverage
  signal_child_burst.c A shell reaping a burst of near-simultaneous child exits
                       (SIGCHLD) must never get stuck forever in sigsuspend()
  eventfd_interrupt.c  eventfd read/poll interruption via the generic wait path
  futex_core.c         FUTEX_WAIT/FUTEX_WAKE timeout, wake, and signal coverage
  process_lifecycle.c  fork/exec/vfork/wait and signal inheritance coverage
  pthread_sync.c       mutex/condvar/rwlock/timed wait and pthread_once coverage
  amd64_regress.c      amd64 cross-page write, exec loader, fcntl race, and cc1 stress
  amd64_gas_probe.sh   host-side GNU as immediate/register encoding probe

All focused tests accept -v or --verbose. Without it they print only failures
plus the final PASS/FAIL line for each test.
