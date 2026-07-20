WebKit-based iSH architecture notes

If iSH were reworked as a WebKit-hosted application, the most plausible design
would be a native iOS shell app with a `WKWebView`, a terminal UI in JS, and
the guest CPU core compiled to Wasm.

High-level shape:

1. Native app shell owns lifecycle, windowing, file import/export, and any
   iOS-specific integration.
2. `WKWebView` hosts the terminal frontend and the emulator runtime.
3. The x86/amd64 CPU core runs in Wasm inside WebKit.
4. Syscalls are handled by a JS shim, then either:
   - mapped onto browser-style storage/network/timer APIs, or
   - bridged back to native code for stronger Unix-like behavior.

Potential advantages:

- Uses WebKit's optimized JS/Wasm engine on iOS.
- Makes the CPU core more portable across browser-capable platforms.
- Keeps terminal rendering and input handling in a well-understood web stack.

Main drawbacks for iSH specifically:

- Syscalls become more expensive because they cross a Wasm/JS and often a
  JS/native boundary.
- PTY/TTY semantics, polling, signals, job control, and fd behavior become
  harder to model correctly.
- Filesystem and socket behavior become less native unless a large native
  bridge is added.
- Many real iSH workloads are syscall-heavy, not just compute-heavy, so the
  bottleneck likely shifts from CPU translation to host-integration overhead.

Practical takeaway:

- WebKit/Wasm could be attractive as a portability experiment.
- It is not an obvious speed win over the current native backend plus ARM64
  JIT/interpreter model.
- The strongest experimental path would be to swap only the CPU execution
  engine behind a narrow ABI first, rather than rewriting the whole host
  interface around a browser model.
