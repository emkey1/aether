# Go Release Smoke TODO

Manual release-prep checklist for Go-heavy regressions under `build/ish`.

## Script

Run:

```bash
tests/manual/go_release_smoke.sh .tmp-go-alpine-x86
```

Useful knobs:

```bash
GEN_COUNT=2  RUN_BACKGROUND_MONITORS=1 tests/manual/go_release_smoke.sh .tmp-go-alpine-x86
GEN_COUNT=16 RUN_BACKGROUND_MONITORS=0 tests/manual/go_release_smoke.sh .tmp-go-alpine-x86
```

## What It Exercises

- cold `go build ./...`
- warm `go build ./...`
- `go test ./...`
- `go run ./cmd/buildmatrix`
- `os/exec` subprocess churn
- loopback `net/http`
- file-tree creation plus `archive/zip`
- background `top` and `ps` sampling during the build

## Current Findings

- The earlier `findScavengeCandidate` crash conclusion was incorrect.
- The saved `e2e_out/go_release_smoke_fail_gen16/*` files are partial snapshots, not proof of a guest runtime crash.
- Live guest-rootfs inspection showed both:
  - a build-only run without monitors continuing past the earlier snapshot point
  - a monitored run with `top` and `ps` also continuing past the earlier snapshot point
- The practical issue was test observability:
  - the original harness only wrote `results/status.txt` on total success
  - a long-running `go build` therefore looked like a hang or crash during spot checks
- The harness now records:
  - `results/phase.txt`
  - `results/phase-updated.txt`
  - `results/status.txt`
  - `results/config.txt`

## Live-Run Guidance

- For long-running builds, inspect the live guest rootfs under:
  - `.tmp-go-alpine-x86*/data/tmp/go-release-smoke/results/`
- Use `phase.txt` and log growth, not copied `e2e_out` snapshots alone, to decide whether a run is truly stuck.

## Open Questions

- Do any of the heavier scenarios eventually fail end-to-end, or are they only slower than expected on cold cache?
- Does background `top`/`ps` materially increase wall-clock time enough to justify a separate timeout budget for monitored runs?
- Are there additional Go workloads that expose correctness issues beyond compile latency?

## Next Steps

- Let the monitored and unmonitored smoke runs finish to completion and record wall-clock expectations.
- Add one or two larger real-world Go workloads once the baseline matrix has reliable completion data.
- Treat future failures as real only if:
  - `status.txt` reports `failed:<phase>`
  - or the live rootfs logs stop advancing for materially longer than the expected compile window.
