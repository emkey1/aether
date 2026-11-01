# Map note for the d5 map re-solve

MiMo re-solved the keyed-data tasks with `docs/aether_for_llms_medium_contexts.md` at tag `1.0` followed by the
section below (the whole guide document: sha256 `39933a9a28be453def61be686859c7d5e0d38689dd66b6bf7f40e6c899288290`, the `doc_sha256` of
`report__aether_mapsolve.json.gz`). Training prompts never contain it.


## Solver note: keyed data

When a task counts, groups, totals or looks things up by a name or code, use a map: `let m: Map[Text, Int] = {};`,
`m[k] = get(m, k, 0) + 1;`, `has(m, k)`, `keys(m)`, `loop k in m { ... }` (insertion order). Prefer a map over parallel
arrays or a record of arrays. For a sorted output, use `sorted(keys(m))`.
