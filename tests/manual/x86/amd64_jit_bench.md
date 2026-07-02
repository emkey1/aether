# amd64 JIT benchmark

Run the host-side benchmark against a local amd64 guest rootfs:

```sh
tests/manual/amd64_jit_bench.sh
```

By default it compares interpreter and JIT runs for:

- `/bin/true`
- `uname -a >/dev/null`
- `busybox --help >/dev/null`
- `apk --help >/dev/null`

For each case it records:

- pass/fail
- wall-clock time

With `-t`, it also records:

- generic `helper-step` count
- direct helper hit count
- bad-target / bad-rip diagnostics
- top helper-step opcodes

Use `-c name=command` to add workload-specific cases, for example:

```sh
tests/manual/amd64_jit_bench.sh -c 'ssh_v=ssh -V >/dev/null'
```

Tracing example:

```sh
tests/manual/amd64_jit_bench.sh -t -c 'apk_help=apk --help >/dev/null'
```
