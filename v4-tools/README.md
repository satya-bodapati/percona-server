# v4-tools — building the v5 split

`vector-persistence-v5` is a pure re-split of `vector-persistence-v2`: same
content, new commit boundaries. The tip must come out byte-identical to v2,
and that is the check to run after every build.

    python3 v4-tools/build-v4.py v4-tools/map.txt <worktree>
    git -C <worktree> diff --quiet vector-persistence-v2 HEAD   # must be empty

## The pieces

| file | what it does |
|---|---|
| `map.txt` | every symbol and file, and the commit it belongs to |
| `build-v4.py` | writes each commit's tree from the map and commits it |
| `symrange.py` | top-level symbol ranges; strips comments and literals, handles attribute prefixes and signatures split over two lines |
| `linkcheck.py` | callers the map places before their callees |
| `touched.py` | which symbols of a file the branch actually changes |
| `symbols-per-commit.py` | what each commit changes, named by function - the view to review a split by |
| `audit.py` | what landed in a commit by decision vs what came along for the ride |
| `coverage.py` | every line v2 adds against every line v5 adds |
| `gen-dict0mem-override.py` | regenerates commit 1's intermediate `dict0mem.h` |
| `aux-sub-trx-to-v2.patch` | what v2 changed after the public PR: the eight fix commits |
| `gate-v4.sh` | per-commit build + clang-format + the vector suite, on the lab box |

## What the builder does about things that resist slicing

A file is emitted at its own step with the oracle's text. Symbols assigned to
a later step fall back to the base tree's version, so nothing upstream is
deleted. On top of that:

- **includes are pruned** - a file appearing early otherwise carries its final
  include list, which drags every header it will ever need in with it;
- **header declarations are pruned** - otherwise a header declares the whole
  module's API from the first commit that touches it;
- **`DEFER <step> <file> <token>`** drops a whole top-level statement naming
  that token until its step. A sysvar registration lives at file scope, so it
  would otherwise arrive with its file rather than with the variable;
- **`overrides/`** supplies a hand-written intermediate where a symbol or file
  carries more than one commit's change:

      overrides/<step>/<path>/<symbol>.txt     one function
      overrides/<step>/<path>                  a whole file (headers, tests)

  An override applies from its step until the symbol's own step arrives, when
  the oracle's text takes over - which is why the tip stays identical.

Current overrides, and why each exists:

- `check_if_supported_inplace_alter` (1) refuses in-place ADD VECTOR INDEX and
  native rebuild. Both need a builder that arrives with the ALTER commit;
  without this, `commit_try_norebuild` asserts on an index that never reached
  `ONLINE_INDEX_COMPLETE`, and `OPTIMIZE TABLE` takes the server down.
- `ha_innobase::open` (1) counts the hidden column when comparing column counts,
  without the runtime loop that arrives with the write path. Without it
  `SHOW CREATE TABLE` reports the table does not exist.
- `dict0mem.h` (1) keeps the base persistent-metadata enum. The header arrives
  whole, so the enum would otherwise grow past the persister registered in the
  next commit, and the base write loop walks onto a null persister - the server
  segfaults during bootstrap, before any test runs.
- `Tester::Tester` (3) dispatches only the interpreter commands whose bodies
  exist yet.
- `vector_persistence.test` / `.result` (4) without the DISCARD section, which
  tests a refusal that arrives with the IMPORT/EXPORT commit.

## Two traps worth knowing

A file with **both** a FILE entry and symbol entries silently ignores the
symbols - the whole file lands at the FILE entry's step. `build-v4.py` refuses
to run on such a map, because the result is a commit carrying another commit's
work and it looks fine until someone reads it.

A gate run that executes **no tests** is not a pass. MTR can fail to start the
server entirely, which leaves `pass=0 fail=0`; the gate scores that RED.
