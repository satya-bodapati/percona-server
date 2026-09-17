# v4-tools — rebuilding the v4 split

Phase A of `vector-persistence-v4` is a pure re-split of `vector-persistence-v2`:
same content, new commit boundaries. The tip must come out byte-identical to v2,
and that is the check to run after every build.

    python3 v4-tools/build-v4.py v4-tools/map.txt <worktree>
    git -C <worktree> diff --quiet vector-persistence-v2 HEAD   # must be empty

## The pieces

| file | what it does |
|---|---|
| `map.txt` | every symbol and file, and the step it belongs to |
| `build-v4.py` | writes each step's tree from the map and commits it |
| `symrange.py` | top-level symbol ranges, comments and literals stripped first |
| `linkcheck.py` | finds callers the map places before their callees |
| `touched.py` | which symbols of a file the branch actually changes |
| `coverage.py` | every line v2 adds against every line v4 adds |
| `overrides/` | hand-written intermediate forms (see below) |
| `gate-v4.sh` | per-commit build + clang-format + the vector suite, on the lab box |

## Overrides

A few functions carry more than one step's change and cannot be sliced. An
override supplies the intermediate form:

    overrides/<step>/<path>/<symbol>.txt     one function
    overrides/<step>/<path>                  a whole file (used for tests)

An override applies from its step until the symbol's own step arrives, when the
oracle's text takes over — which is why the tip stays identical. Current ones:

- `check_if_supported_inplace_alter` at step 2 — refuses in-place ADD VECTOR
  INDEX and native rebuild, both of which need a builder that arrives at step 8.
- `ha_innobase::open` at step 2 — counts the hidden column when comparing column
  counts, without the runtime loop that arrives at step 4.
- `Tester::Tester` at step 3 — dispatches only the two commands whose bodies
  exist yet.
- `vector_persistence.test` / `.result` at step 4 — without the DISCARD section,
  which needs the refusal from step 10.
