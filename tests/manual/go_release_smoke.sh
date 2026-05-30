#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

ISH_BIN="${ISH_BIN:-./build/ish}"
ROOTFS="${1:-.tmp-go-alpine-x86}"
OUTDIR="${OUTDIR:-e2e_out/go_release_smoke_$(date +%Y%m%d_%H%M%S)}"
HOST_WORK="$(mktemp -d "${TMPDIR:-/tmp}/go-release-smoke.XXXXXX")"
GUEST_WORK="/tmp/go-release-smoke"
GEN_COUNT="${GEN_COUNT:-16}"
RUN_BACKGROUND_MONITORS="${RUN_BACKGROUND_MONITORS:-1}"

cleanup() {
    rm -rf "$HOST_WORK"
}
trap cleanup EXIT

mkdir -p "$OUTDIR"

cat >"$HOST_WORK/go.mod" <<'EOF'
module example.com/go-release-smoke

go 1.25
EOF

mkdir -p "$HOST_WORK/internal/compute" "$HOST_WORK/internal/pipeline"
mkdir -p "$HOST_WORK/cmd/buildmatrix" "$HOST_WORK/cmd/execstorm" "$HOST_WORK/cmd/httploop" "$HOST_WORK/cmd/fswalk"

cat >"$HOST_WORK/internal/compute/compute.go" <<'EOF'
package compute

import (
	"slices"
	"strings"
	"sync"
)

func ParallelSum(values []int, workers int) int {
	if workers < 1 {
		workers = 1
	}
	if len(values) == 0 {
		return 0
	}
	if workers > len(values) {
		workers = len(values)
	}
	step := (len(values) + workers - 1) / workers
	var wg sync.WaitGroup
	results := make(chan int, workers)
	for start := 0; start < len(values); start += step {
		end := start + step
		if end > len(values) {
			end = len(values)
		}
		wg.Add(1)
		chunk := slices.Clone(values[start:end])
		go func() {
			defer wg.Done()
			sum := 0
			for _, v := range chunk {
				sum += v
			}
			results <- sum
		}()
	}
	go func() {
		wg.Wait()
		close(results)
	}()
	total := 0
	for partial := range results {
		total += partial
	}
	return total
}

func WordHistogram(lines []string) map[string]int {
	out := map[string]int{}
	for _, line := range lines {
		for _, word := range strings.Fields(strings.ToLower(line)) {
			out[word]++
		}
	}
	return out
}
EOF

cat >"$HOST_WORK/internal/compute/compute_test.go" <<'EOF'
package compute

import "testing"

func TestParallelSum(t *testing.T) {
	values := make([]int, 1024)
	want := 0
	for i := range values {
		values[i] = i + 1
		want += i + 1
	}
	if got := ParallelSum(values, 9); got != want {
		t.Fatalf("ParallelSum=%d want %d", got, want)
	}
}
EOF

cat >"$HOST_WORK/internal/pipeline/pipeline.go" <<'EOF'
package pipeline

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"io"
)

func RoundTrip(payload []byte) (string, int, error) {
	var compressed bytes.Buffer
	gzw := gzip.NewWriter(&compressed)
	if _, err := gzw.Write(payload); err != nil {
		return "", 0, err
	}
	if err := gzw.Close(); err != nil {
		return "", 0, err
	}

	gzr, err := gzip.NewReader(bytes.NewReader(compressed.Bytes()))
	if err != nil {
		return "", 0, err
	}
	defer gzr.Close()

	restored, err := io.ReadAll(gzr)
	if err != nil {
		return "", 0, err
	}
	sum := sha256.Sum256(restored)
	return hex.EncodeToString(sum[:]), len(restored), nil
}
EOF

cat >"$HOST_WORK/internal/pipeline/pipeline_test.go" <<'EOF'
package pipeline

import (
	"strings"
	"testing"
)

func TestRoundTrip(t *testing.T) {
	hash, size, err := RoundTrip([]byte(strings.Repeat("release-smoke-", 256)))
	if err != nil {
		t.Fatal(err)
	}
	if len(hash) != 64 {
		t.Fatalf("hash length=%d", len(hash))
	}
	if size != len(strings.Repeat("release-smoke-", 256)) {
		t.Fatalf("size=%d", size)
	}
}
EOF

for i in $(seq 1 "$GEN_COUNT"); do
    pkg="$(printf 'gen%02d' "$i")"
    next=""
    if [ "$i" -lt "$GEN_COUNT" ]; then
        next="$(printf 'example.com/go-release-smoke/internal/generated/gen%02d' $((i + 1)))"
    fi
    mkdir -p "$HOST_WORK/internal/generated/$pkg"
    {
        printf 'package %s\n\n' "$pkg"
        printf 'import (\n'
        printf '\t"fmt"\n\t"strings"\n'
        if [ -n "$next" ]; then
            printf '\tchild "%s"\n' "$next"
        fi
        printf ')\n\n'
        printf 'func Value(seed int) string {\n'
        printf '\tbase := fmt.Sprintf("pkg:%s:%%d", seed)\n' "$pkg"
        printf '\tbase = strings.ToUpper(base)\n'
        if [ -n "$next" ]; then
            printf '\treturn base + ":" + child.Value(seed+1)\n'
        else
            printf '\treturn base\n'
        fi
        printf '}\n'
    } >"$HOST_WORK/internal/generated/$pkg/$pkg.go"
done

cat >"$HOST_WORK/cmd/buildmatrix/main.go" <<'EOF'
package main

import (
	"encoding/json"
	"fmt"
	"strings"

	"example.com/go-release-smoke/internal/compute"
	gen01 "example.com/go-release-smoke/internal/generated/gen01"
	"example.com/go-release-smoke/internal/pipeline"
)

func main() {
	values := make([]int, 4096)
	for i := range values {
		values[i] = (i % 97) + 1
	}
	lines := []string{
		"go build release smoke",
		"go build release smoke",
		"process churn procfs tty poll webview",
	}
	hist := compute.WordHistogram(lines)
	hash, size, err := pipeline.RoundTrip([]byte(strings.Repeat(gen01.Value(7)+"|", 300)))
	if err != nil {
		panic(err)
	}
	out := map[string]any{
		"sum":   compute.ParallelSum(values, 13),
		"hash":  hash[:16],
		"size":  size,
		"words": hist["go"] + hist["release"] + hist["smoke"],
	}
	blob, _ := json.Marshal(out)
	fmt.Println(string(blob))
}
EOF

cat >"$HOST_WORK/cmd/execstorm/main.go" <<'EOF'
package main

import (
	"bytes"
	"fmt"
	"os/exec"
	"strings"
	"sync"
)

func main() {
	const jobs = 6
	var wg sync.WaitGroup
	results := make(chan string, jobs)
	for i := 0; i < jobs; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			cmd := exec.Command("/bin/sh", "-lc", "i=1; while [ $i -le 80 ]; do echo job-"+fmt.Sprint(id)+"-$i; i=$((i+1)); done")
			var out bytes.Buffer
			cmd.Stdout = &out
			cmd.Stderr = &out
			if err := cmd.Run(); err != nil {
				results <- fmt.Sprintf("job=%d error=%v", id, err)
				return
			}
			results <- fmt.Sprintf("job=%d lines=%d", id, len(strings.Split(strings.TrimSpace(out.String()), "\n")))
		}(i)
	}
	wg.Wait()
	close(results)
	for result := range results {
		fmt.Println(result)
	}
}
EOF

cat >"$HOST_WORK/cmd/httploop/main.go" <<'EOF'
package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	var count atomic.Int64
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/ping", func(w http.ResponseWriter, r *http.Request) {
		n := count.Add(1)
		fmt.Fprintf(w, "pong-%d", n)
	})

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	defer ln.Close()

	server := &http.Server{Handler: mux}
	defer server.Close()
	go server.Serve(ln)

	client := &http.Client{Timeout: 3 * time.Second}
	var wg sync.WaitGroup
	for i := 0; i < 24; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			resp, err := client.Get("http://" + ln.Addr().String() + "/v1/ping")
			if err != nil {
				panic(err)
			}
			defer resp.Body.Close()
			body, err := io.ReadAll(resp.Body)
			if err != nil {
				panic(err)
			}
			fmt.Println(string(body))
		}()
	}
	wg.Wait()
	fmt.Printf("served=%d\n", count.Load())
}
EOF

cat >"$HOST_WORK/cmd/fswalk/main.go" <<'EOF'
package main

import (
	"archive/zip"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

func main() {
	root, err := os.MkdirTemp("", "go-release-smoke-fs")
	if err != nil {
		panic(err)
	}
	defer os.RemoveAll(root)

	for i := 0; i < 40; i++ {
		dir := filepath.Join(root, fmt.Sprintf("dir-%02d", i%7))
		if err := os.MkdirAll(dir, 0o755); err != nil {
			panic(err)
		}
		path := filepath.Join(dir, fmt.Sprintf("file-%02d.txt", i))
		if err := os.WriteFile(path, []byte(fmt.Sprintf("payload-%02d", i)), 0o644); err != nil {
			panic(err)
		}
	}

	zipPath := filepath.Join(root, "bundle.zip")
	zf, err := os.Create(zipPath)
	if err != nil {
		panic(err)
	}
	zw := zip.NewWriter(zf)
	files := 0
	err = filepath.WalkDir(root, func(path string, d os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if d.IsDir() || path == zipPath {
			return nil
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		w, err := zw.Create(rel)
		if err != nil {
			return err
		}
		f, err := os.Open(path)
		if err != nil {
			return err
		}
		defer f.Close()
		if _, err := io.Copy(w, f); err != nil {
			return err
		}
		files++
		return nil
	})
	if err != nil {
		panic(err)
	}
	if err := zw.Close(); err != nil {
		panic(err)
	}
	if err := zf.Close(); err != nil {
		panic(err)
	}
	fmt.Printf("files=%d zip=%s\n", files, zipPath)
}
EOF

cat >"$HOST_WORK/run-smoke.sh" <<'EOF'
#!/bin/sh
set -eu

cd /tmp/go-release-smoke
rm -rf results
mkdir -p results/bin
export GOCACHE=/tmp/gocache-go-release-smoke
export GOTMPDIR=/tmp/gotmp-go-release-smoke
mkdir -p "$GOCACHE" "$GOTMPDIR"

TOP_PID=""
PS_PID=""
CURRENT_PHASE="init"

phase() {
    CURRENT_PHASE="$1"
    printf '%s\n' "$CURRENT_PHASE" > results/phase.txt
    date '+%Y-%m-%d %H:%M:%S %z' > results/phase-updated.txt
}

if [ "${RUN_BACKGROUND_MONITORS:-1}" = "1" ]; then
    (
        while :; do
            top -b -n 1 | head -20
            sleep 1
        done
    ) > results/top.log 2>&1 &
    TOP_PID=$!

    (
        while :; do
            date
            ps -o pid,ppid,stat,args
            sleep 1
        done
    ) > results/ps.log 2>&1 &
    PS_PID=$!
fi

cleanup() {
    status="failed:$CURRENT_PHASE"
    if [ "$CURRENT_PHASE" = "done" ]; then
        status="ok"
    fi
    printf '%s\n' "$status" > results/status.txt
    [ -n "$TOP_PID" ] && kill "$TOP_PID" 2>/dev/null || true
    [ -n "$PS_PID" ] && kill "$PS_PID" 2>/dev/null || true
    [ -n "$TOP_PID" ] && wait "$TOP_PID" 2>/dev/null || true
    [ -n "$PS_PID" ] && wait "$PS_PID" 2>/dev/null || true
}
trap cleanup EXIT

printf 'GEN_COUNT=%s\n' "${GEN_COUNT:-unknown}" > results/config.txt
printf 'RUN_BACKGROUND_MONITORS=%s\n' "${RUN_BACKGROUND_MONITORS:-1}" >> results/config.txt

go env > results/go-env.txt

phase clean-cache
go clean -cache
phase build-cold
go build -x ./... > results/build-cold.log 2>&1
phase build-warm
go build ./... > results/build-warm.log 2>&1
phase test
go test ./... > results/test.log 2>&1

phase build-binaries
go build -o results/bin/buildmatrix ./cmd/buildmatrix
go build -o results/bin/execstorm ./cmd/execstorm
go build -o results/bin/httploop ./cmd/httploop
go build -o results/bin/fswalk ./cmd/fswalk

phase run-buildmatrix
results/bin/buildmatrix > results/buildmatrix.out
phase run-execstorm
results/bin/execstorm > results/execstorm.out
phase run-httploop
results/bin/httploop > results/httploop.out
phase run-fswalk
results/bin/fswalk > results/fswalk.out
phase run-go-run
go run ./cmd/buildmatrix > results/go-run-buildmatrix.out

phase done
EOF
chmod +x "$HOST_WORK/run-smoke.sh"

tar -cf - -C "$HOST_WORK" . | "$ISH_BIN" -f "$ROOTFS" /bin/sh -lc "rm -rf '$GUEST_WORK' && mkdir -p '$GUEST_WORK' && tar -xf - -C '$GUEST_WORK'"
mkdir -p "$OUTDIR"

guest_status=0
if ! "$ISH_BIN" -f "$ROOTFS" /bin/sh -lc "cd '$GUEST_WORK' && GEN_COUNT='$GEN_COUNT' RUN_BACKGROUND_MONITORS='$RUN_BACKGROUND_MONITORS' sh ./run-smoke.sh"; then
    guest_status=$?
fi

if "$ISH_BIN" -f "$ROOTFS" /bin/sh -lc "[ -d '$GUEST_WORK/results' ]"; then
    "$ISH_BIN" -f "$ROOTFS" /bin/tar -cf - -C "$GUEST_WORK/results" . | tar -xf - -C "$OUTDIR"
fi

echo "wrote results to $OUTDIR"
exit "$guest_status"
