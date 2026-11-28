# Pin moves since 1.0 without a gate summary, sorted (2026-11-28)

Roadmap step 3(b), E1's step 0: the evidence Q8 waits on. At `c329db8`, 138
aether commits since the `1.0` tag moved an `external/` gitlink and 54 of them
mention `pin_gate`. This sorts the other 84 by whether an engine commit in the
range they moved can change an Aether program on the default build (macOS or
Linux; SDL off, the default; curl on).

## Result

| Pin moves (84) | |
|---|---|
| Aether-visible: at least one engine commit in the range is | **20** |
| SDL build only: touches only the SDL backend, which the default build leaves out | 6 |
| Other front end: the exsh shell, the iOS app, Pascal, CLike, Rea's own tests and docs | 57 |
| No engine change: `2acdb1b` repairs `c521ffb`'s gitlink, which named no commit | 1 |

| Engine commits in those ranges (128) | Decided by | |
|---|---|---|
| Touch no file the aether target compiles or includes | file list | 58 other front end, 6 SDL only |
| Touch such a file, but every aether translation unit that reads it preprocesses identically before and after | preprocessing | 17 other front end |
| Change what an aether translation unit compiles to | reading | 26 Aether-visible, 21 other front end |

None of the 20 Aether-visible moves bumped `VERSION` in its own commit. The next
`VERSION` change after 18 of them is `b464e0a` (2026-11-18), after the other two
`830a6d8` (2026-11-24).

"Aether-visible" means the changed code runs in an Aether program, not that its
behaviour changed. About ten of the 26 are refactors, an optimisation or a
builtin-id shift the reading judged behaviour-preserving: `066f336`, `b9b74e8`,
`4fedf06`, `4bbbd9d`, `688e1ac`, `9c588a1`, and the static dispatch-table
additions `2dcd443`, `d096e4d`, `a835aa4` and `23e3bb9`, which move the id of
every registered builtin after them. A pin gate is what would catch one of those
going wrong. The rest change something a program can observe. Examples: Ctrl-C
exits 130 (`5247bc5`, `a45342f`, `d145b33`, `76f08e5`); `socket_poll` runs on
`poll` (`2378585`); `use` also searches `<exe>/../lib/rea` (`c6e5598`, `b55acdb`); a
non-regular source skips the bytecode cache (`3394406`); `fopen` takes a char
(`eba2db5`); `pollkeyany` is registered without SDL (`d51e663`); thread jobs take
real copies (`4f99309`).

Subjects mislead in both directions. Six of the 26 visible commits begin "ios:",
and 21 commits that change compiled code turned out to add only declarations or
stubs that Aether never calls.

| Visible pin move | Engine commits that are visible |
|---|---|
| `952f9e4` | `066f336`, `b9b74e8` (of 4) |
| `a6f0d6b` | `2378585` |
| `e5cbef6` | `b55acdb`, rea `c6e5598` |
| `d0df102` | `4fedf06` |
| `b3da29a` | `3394406` |
| `666db07` | `7a45cb8` |
| `aa6d90c` | `0aaefb2` |
| `e2ad836` | `4bbbd9d` |
| `d1b5835` | `23e3bb9` (of 2) |
| `418dd0b` | `688e1ac` |
| `3a7d63f` | `9c588a1` |
| `42f9024` | `65b4a37`, `d145b33`, `b26d201`, `a45342f`, rea `5247bc5` (of 10) |
| `728e5da` | `d64986c` |
| `ff10cdc` | `eba2db5` |
| `607be45` | `76f08e5` |
| `61fee48` | `2dcd443` |
| `bf54831` | `d096e4d` |
| `0cd64e5` | `a835aa4` |
| `37622f8` | `4f99309` |
| `f2f74b1` | `d51e663` |

The SDL-only moves are `63d76ff`, `1e59f8d`, `33f2311`, `f87d34e`, `4125012` and
`0cac11f`: Aether's SDL build sees them, the benchmark's build does not.
`pin_moves_since_1.0_2026-11-28.json` has every engine commit with its class,
the pass that decided it, and the reason.

## How

1. **List** (git only). `pin_moves_census.py list` takes every commit in
   `1.0..c329db8` that changed an `external/` gitlink and whose message does not
   mention `pin_gate`, the engine range each move spans, and each engine
   commit's files.
2. **Files.** `pin_moves_aether_deps_c329db8.txt` holds the 163 engine files
   the aether target reads, sources and headers, taken from the depfiles of a
   Release build of `c329db8` with SDL off. pscal-core's source list has no
   platform branch outside SDL, so Linux compiles the same sources.
   `pin_moves_census.py firstpass` marks an engine commit that touches none of
   them as outside the build. An outside commit that touches only SDL backend
   files is counted separately.
3. **Preprocessing.** `pin_moves_preproc_diff.py` preprocesses every aether
   translation unit that reads a touched file. It uses that unit's own command
   from `compile_commands.json` and runs once at the engine commit's parent and
   once at the commit, in one scratch worktree. Comments and line markers drop
   out (`-E -P`); `__LINE__`, `__FILE__`, `__DATE__` and `__TIME__` are pinned;
   blank lines and spacing are normalised. Identical output in every unit means
   the commit cannot change the aether binary, whether it is a comment, `#if`-ed
   out or for another platform. The verdicts are in
   `pin_moves_preproc_2026-11-28.json`.
4. **Reading.** The 47 commits whose preprocessed code changed were read
   against their diffs by three readers, in groups of 16, 16 and 15, all under one
   rubric. "Aether-visible": the change can alter what an Aether program
   compiles to, how it runs, what it prints, its exit status or signals, or its
   diagnostics, counting behaviour-preserving changes to code an Aether program
   executes. "Other front end": the changed code runs only for another front
   end or the iOS app, or only adds declarations, fields or stubs that no code
   an Aether program runs reads. Each verdict names the guard or call path that
   decides it (`pin_moves_reading_2026-11-28.json`); none came back unsure. One
   verdict was checked again by hand (`d64986c`, against its diff and
   `vmFrontendHidesShellBuiltins`).
5. **Roll-up.** `pin_moves_census.py rollup` classes a pin move by its most
   visible engine commit.

```sh
python3 results/decisions/pin_moves_census.py list . 1.0 c329db8 /tmp/pm/list.json
python3 results/decisions/pin_moves_census.py firstpass /tmp/pm/list.json \
  results/decisions/pin_moves_aether_deps_c329db8.txt /tmp/pm/classed.json
# a build of c329db8 configured with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON, in build/
python3 results/decisions/pin_moves_preproc_diff.py . build /tmp/pm/classed.json /tmp/pm/preproc.json
python3 results/decisions/pin_moves_census.py rollup /tmp/pm/classed.json \
  results/decisions/pin_moves_preproc_2026-11-28.json \
  results/decisions/pin_moves_reading_2026-11-28.json \
  results/decisions/pin_moves_since_1.0_2026-11-28.json
```

## Limits

- The reading is judgment, not a run. Two calls are borderline. `82cec95`'s
  one unguarded line acts only after a compile error, on a path Aether's
  semantic pass rules out, so it counts as other front end. `65b4a37` reaches
  Aether only with `PSCAL_INIT_TERM` set, so it counts as visible.
- The file and preprocessing passes use today's dependency list and today's
  compile flags at every commit, so a file the build read then but not now
  would be missed. Only pscal-core's `CMakeLists.txt` changes the target's
  sources, and in these ranges one commit changes it: `e1619ad` adds
  `common/net_snapshot.c`, read as other front end because only the
  `/proc/net` writer calls it.
- The 54 moves that mention `pin_gate` were not checked here, and neither was
  what their gate compared.
