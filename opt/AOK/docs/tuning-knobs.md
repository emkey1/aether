# Runtime tuning: CPU count and memory headroom

A couple of environment variables let you tune how iSH-AOK presents itself
to the guest. These apply when you can control the process environment —
building and running the standalone CLI emulator, or launching the app
from Xcode with a custom scheme environment — rather than something an
App Store install lets you change day to day.

## `ISH_GUEST_CPU_COUNT`

Overrides the CPU count the guest scheduler is told about (what `nproc`
and `sched_getaffinity` report inside the guest).

By default, iSH-AOK reserves roughly a third of the host's cores back from
the guest rather than exposing every core iOS reports. Set
`ISH_GUEST_CPU_COUNT` to override that:

```sh
ISH_GUEST_CPU_COUNT=6 ./ish -f build/alpine /bin/sh   # match a specific core count
ISH_GUEST_CPU_COUNT=1 ./ish -f build/alpine /bin/sh   # force a serial (single-core) guest
```

Forcing `=1` is particularly useful when debugging a concurrency bug: it
rules out cross-core races as the cause by construction.

## `ISH_GUEST_MEM_HEADROOM_MB`

Sets the free-memory threshold (in MB) below which iSH-AOK's memory
pressure guard kicks in and starts throttling new guest allocations.
Defaults to 192 MB. Set to `0` to disable the guard entirely:

```sh
ISH_GUEST_MEM_HEADROOM_MB=0 ./ish -f build/alpine /bin/sh
```

## Logging

Log channel selection (`strace`, `verbose`, `instr`, and friends) is a
**build-time** setting, controlled via `ISH_LOG` in `app/iSH.xcconfig` for
the iOS app, or `meson configure -Dlog=...` for the CLI — see the main
project README's "Logging and Diagnostics" section for the full list of
channels.
