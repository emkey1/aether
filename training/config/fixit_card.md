# Aether: rules a fine-tuned model most often misses

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
4. **There are no character-class builtins** (`is_digit`, `is_alpha`, `isspace` do not exist). Compare character
   codes instead: `let c: Int = ord(s[i]); c >= ord("0") && c <= ord("9")`.
5. **Memory streams:** `mstream_new()`, `mstream_write_text(ms, t)`, `mstream_write_byte(ms, b)`,
   `mstream_text(ms)`, `mstream_free(ms)`. Nothing else exists.
