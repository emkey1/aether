# Go Release Matrix

Repeatable release-prep matrix for Go-heavy validation on `build/ish`.

## Script

Run:

```bash
tests/manual/go_release_matrix.sh .tmp-go-alpine-x86-pass
```

This creates a timestamped directory under `e2e_out/` containing:

- `summary.tsv`
- `SUMMARY.md`
- `smoke/`
- one directory per external repo case

## Default Cases

The matrix currently runs:

1. synthetic smoke workload via [go_release_smoke.sh](/Users/mke/git/ish-AOK/tests/manual/go_release_smoke.sh)
2. `rakyll/hey`
3. `spf13/cobra-cli`

External repos are vendored on the host first, then copied into a fresh guest rootfs and built with:

- `HOME=/tmp`
- `GOCACHE=/tmp/<case>-gocache`
- `GOFLAGS=-mod=vendor`

## Useful Knobs

```bash
SMOKE_GEN_COUNT=1 SMOKE_MONITORS=0 tests/manual/go_release_matrix.sh .tmp-go-alpine-x86-pass
OUTDIR=e2e_out/go_release_matrix_quick tests/manual/go_release_matrix.sh .tmp-go-alpine-x86-pass
MATRIX_CASES="hey cobra-cli" tests/manual/go_release_matrix.sh .tmp-go-alpine-x86-pass
HOST_GO_BIN=/opt/homebrew/bin/go HOST_GOROOT=/opt/homebrew/Cellar/go/1.26.2/libexec tests/manual/go_release_matrix.sh .tmp-go-alpine-x86-pass
```

## Interpretation

- `ok`: case finished and the verify command ran successfully
- `failed`: guest build or verify command failed
- synthetic smoke may still be long-running on cold cache; check its `status.txt` and `build-cold.log`

## Notes

- The external-project lane is intentionally build-first, not full test-suite-first.
- External verification is deliberately minimal:
  - the built binary must exist and be executable
  - command-line behavior like `-h` or `--help` is not treated as a release gate
- This is meant to catch:
  - long compile regressions
  - guest process lifecycle issues
  - poll/futex/TTY regressions that show up under realistic Go workloads
