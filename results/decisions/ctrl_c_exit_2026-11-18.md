# Exit status of an interrupted program (D89), 2026-11-18

**Question.** A program stopped by Ctrl-C exited 0 from a running loop under
`1.0`, and `main` exits 130. Keep 130, or restore the `1.0` statuses?

**Method.** Start each program in the background, wait two seconds, send it
SIGINT, and read the status with `wait`. The blocked program reads from a pipe
that stays open.

```aether
// loop.aether
fn main() -> Void { let n = 0; loop { n = n + 1; if n < 0 { break; } } }
```

```aether
// blocked.aether
fn main() -> Void { fx { let s: Text = read_line(); println(s); } }
```

```bash
aether --no-cache loop.aether > out.txt 2> err.txt &
pid=$!; sleep 2; kill -INT $pid; wait $pid; echo "status=$?"

(sleep 30 | aether --no-cache blocked.aether > out.txt 2> err.txt) &
pid=$!; sleep 2; pkill -INT -f "aether --no-cache blocked.aether"; wait $pid; echo "status=$?"
```

| binary | `loop.aether` | `blocked.aether` | stdout, stderr |
|---|---|---|---|
| `2026-10-30-1 (1.0)`, the release binary | 0 | 1 | empty, empty |
| `2026-11-18-1 (1.01-dev)`, `main` | 130 | 130 | empty, empty |

Measured on the build machine (macOS, arm64) on 2026-11-18 and again on
2026-11-24 with the same result.

**Reading.** Under `1.0` an interrupted loop reported success. Any caller that
reads the status (a script, a grader, CI) took the run as finished. 130 is what
a shell reports for a death by SIGINT, and it is the same for both programs.
No program's output changes: the status is the only difference. The owner kept
130 (D89); language `2026-11-18-1` records it.
