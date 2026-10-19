# Aether: rules a fine-tuned model most often misses

Drawn from run 6's main-suite misses. Given as a guide to run 6 it made no difference (68 vs 69 of 73),
and it is not used by any run. Training with a card in the system turn is untested.

1. **File and TOON reads are effects.** `toon_parse_file`, `read_file`, `read_lines` and `mstream_load` go
   inside `fx { }`. Declare the binding before the block and assign it inside, because a `let` inside `fx` is not
   visible after the block:
   ```
   let doc: ToonDoc;
   fx { doc = toon_parse_file("data.json"); }
   ```
2. **TOON arrays are nodes, not Aether arrays.** Loop with `toon_len(node)` and read element `i` with
   `toon_at(node, i)`. Never write `node[i]`.
3. **Arrays are copied into functions.** When a helper changes an array, the caller's copy stays the same. The
   helper returns the new array and the caller reassigns it: `xs = sorted_copy(xs);`
4. **Character classes** (since 2026-10-19-1, D67): `is_digit`, `is_alpha`, `is_alnum`, `is_space`, `is_upper` and
   `is_lower` take `Text` and are true when it is non-empty and every character is in the class (`is_digit(s[i])`).
   Before D67 they did not exist; run 6 invented `is_digit`.
5. **Memory streams:** `mstream_new()`, `mstream_write_text(ms, t)`, `mstream_write_byte(ms, b)`,
   `mstream_text(ms)`, `mstream_free(ms)`. Nothing else exists.
