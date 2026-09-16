# v3: rebuild the branch into ~10 category commits

## Why v3 can be verified far harder than v2 was

v2 was built by surgery on an existing chain: amend, fixup, revert, rebase.
Every mistake today came from that shape - a hunk deleted with its neighbour,
a hunk landing in the wrong commit, a gate result outliving the rewrite that
invalidated it.

v3 is a different problem. The final tree already exists and is verified:
`260189a8546`, 26 commits, every one building, clang-format clean and passing
54 tests. v3 only has to **redistribute that exact tree** across fewer commits.
That gives an invariant v2 never had: **the v3 tip must be byte-identical to
the v2 tip.** Anything lost, duplicated or invented shows up as a diff.

## Phase 0 - freeze the reference

    git tag v2-verified 260189a8546        # never moves, never deleted
    git branch v2-fallback  260189a8546

v2 stays intact until v3 gates green. The tag is the oracle for every check
below.

## Phase 1 - the assignment map, before any commit exists

Let `D = diff(base .. v2-verified)` - 185 files, 18454 insertions, 242
deletions. Split D into N category patches such that every hunk is assigned to
exactly one category.

- Most files belong wholly to one category. Assign at **file** granularity
  wherever possible; it removes judgement and the chance of a split error.
- Only genuinely shared files - `handler0alter.cc`, `ha_innodb.cc`,
  `vec0hnsw.cc`, `dict0dd.cc` - need **hunk** granularity.
- The map is data: `category, file, hunk-id`. It is reviewable on its own,
  before a single commit is made.

**Gate the map, not the history.** Apply categories 1..N onto base in order in
a throwaway worktree and compare the result to `v2-verified`. If it is not
identical, the map is wrong - fix the map, not the commits. No history is
written until this passes.

## Phase 2 - dependency order

Derive the order rather than guess it. For each category, collect the symbols
it *defines* and the symbols it *references*; category Y must follow X if Y
references a symbol X defines. Use the codebase graph (`search_graph`,
`trace_path`) for this, not grep.

Two orderings are then checked mechanically: the build proves symbol
availability, and the suite proves behavioural availability (a test cannot
assert something its category has not yet delivered - the `[1]` INSERT crash
and the `[18]` rebuild-refusal were both this).

Tests go in the category that *completes* the behaviour they assert, and a
test that spans categories gets split.

## Phase 3 - guard.sh: local, seconds, runs before every push

| # | check | the mistake it catches |
|---|---|---|
| G1 | `git diff --quiet v2-verified v3-tip` - **byte-identical tips, the primary invariant** | anything lost or invented |
| G2 | sum of per-commit insertions+deletions == D's | a hunk added then reworked later, i.e. misplaced |
| G3 | every `.cc`/`.h` in every commit: braces balance, file ends with `}` | truncation, at the moment it happens - **not** over-removal, which keeps braces balanced |
| G4 | clang-format drift == 0 per commit | format churn |
| G5 | every added `.test` has its `.result` in the same commit | half-added tests |
| G6 | no commit deletes a file an earlier v3 commit added | misassignment |
| G7 | `git blame` of the tip maps each line to the category the map assigns | a hunk in the wrong commit while the tip still matches |
| G8 | commit count and subjects match the plan | silent drift |

G1+G2+G7 together are what v2 lacked. G1 alone would not have caught the
`vec_check_aux_refs` leak - the tip was fine, the hunk was simply in the wrong
commit. G7 catches exactly that.

## Phase 4 - gate.sh: remote, per commit

Unchanged in substance from today's gate - build with `-Werror`,
clang-format, full suite with `--force --max-test-fail=0` - plus these rules,
each one a scar:

- **R1** No `git clean`, no `rm -rf` inside the repo from automation. The build
  directory is untracked; `git clean -qfd` deleted it and cost a full rebuild.
- **R2** One gate at a time, enforced by `flock`. Two concurrent gates
  corrupted the log and left a half-written `.so` that read as a link failure.
- **R3** Never `scp` into the remote worktree. Transfer via branches only.
  Untracked files there collided with `git checkout` twice.
- **R4** A gate result is bound to the SHA it ran on. The script records it;
  the guard refuses to report green for any other SHA. **Any rewrite voids the
  result.**
- **R5** Before calling a failure pre-existing, reproduce it on an untouched
  reference branch. Testing commit N on top of an amended M proves nothing.
- **R6** Before asserting a mechanism, instrument it. `ctx->old_table` looked
  right and was freed memory.
- **R7** Never resolve a rebase conflict by taking a whole file from another
  branch. For tree-wide transforms use `filter-branch --tree-filter`, which
  cannot conflict.
- **R8** Record a `.result` only after reading the output.
- **R9** Never `git commit --no-verify`. Format the staged patch with the same
  tool the hook uses, which also stages the fix:

      git add -u
      ~/scripts/githooks/apply-format --apply-to-staged
      git commit -m "..."

  The hook then has nothing to complain about and never prompts. It stays as
  the backstop rather than something to be pre-empted or bypassed. If the hook
  does prompt (it should not), answer it non-interactively with
  `PRE_COMMIT_HOOK_TTY=~/.claude/precommit-answers`, and for the rare merge
  commit answer `c` and look, since `a` can reformat code the merge never
  touched.
- **R10** A check whose output you read past is not a check. If clang-format,
  the guard or the gate reports anything, stop and fix it before committing.

## Phase 5 - cutover

v3 replaces v2 only after the full gate is green on every commit and G1-G8
pass. Until then v2-fallback is the branch that ships.

## What these checks cannot do

**Over-removal is not caught by G3.** Deleting a whole extra function keeps
braces balanced, so the file still parses. That failure is caught by the build
(undefined symbol) and by reading the diff's first and last removed lines -
which is now part of any removal, not an optional review.

They catch loss, duplication, misordering and misplacement. They cannot catch
a hunk that compiles and passes in the *wrong but plausible* category - that is
a judgement call, and the defence is file-level exclusivity plus a read of each
category's diff before gating. Worth being honest that this residue exists.

## The category list - 10 commits

Derived from v2's 26. The "review fix" commits ([11] [13] [16] [20] [26]) do
not survive as commits: they dissolve into whatever they fixed, which is the
point of v3.

| # | category | absorbs |
|---|----------|---------|
| 1 | Aux table lifecycle: create, drop, rename, truncate, naming, predicates, MDL | [1] [12] [13] |
| 2 | Hidden column `percona_vec_aux_id` **and its label counter**: add, stamp, map, mint, persist in the DD, survive ALTER | [2] [12] [18] |
| 3 | Index syntax: option parsing, DDL validation, `M` bounds | [3] |
| 4 | Vector runtime **and its memory budget**: graph, arena, allocator, persistor, `innodb_hnsw_max_memory` | [4] [14] [9] [24] |
| 5 | Write path: INSERT/UPDATE, aux sub-transaction and commit policy, PK-changing UPDATE, lifetime fixes | [5] [6] [11] [20] |
| 6 | Index build: ADD INDEX population, ddl::Builder, in-memory build then one-pass aux write | [8] [15] [17] |
| 7 | Read path: load, k-NN search, optimizer (COUNT(*), ORDER BY DISTANCE LIMIT) | [8] [10] |
| 8 | ALTER / DDL policy: rebuild allowed, INSTANT and LOCK=NONE refused, hidden column as the marker, DISCARD/IMPORT/EXPORT, row0log exemption | [19] [16] [23] |
| 9 | Error handling: `DB_INDEX_CORRUPT`, `corrupted_hnsw`, `DB_ANN_NODE_NOT_FOUND`/`HA_ERR_ANN_FAILED`, aux-drop failures surfaced, `vec_build_start`'s real error | [21] [22] [20] |
| 10 | Design document | [25] |

CHECK TABLE is **not** a category - reverted, absent from the tree (0
references). If it returns after proper testing it becomes 11.

### Why the budget is not its own category

Not a judgement call - the code decides it. `Vec_arena::global_bytes()` and
`Vec_arena::recount()` are methods *of the arena class*, so the budget
accounting lives inside the thing category 4 introduces. A re-derivation puts
each symbol in exactly one commit in final form, and a function cannot be split
across commits, so the budget cannot be lifted out. What could stand alone is
`srv_hnsw_max_memory` and the plugin variable - about nine lines. Too thin to
be a category.

The label counter merges into the hidden column for the ordinary reason: it
exists only to mint values for that column and they are persisted together.

### The rule that makes a re-derivation work

**A symbol goes in the earliest category that needs it to compile. Its tests go
in the category where the behaviour it asserts is complete.** The two are often
different commits, and that is correct rather than a smell: `vec_runtime_load_once`
carries its corruption handling from the moment it appears, because it must
compile for the write path, while the test that proves a corrupt graph fails
cleanly belongs with error handling.

### The dependency tension to settle in Phase 2

The counter's *code* belongs
  in category 2, but its rebuild-survival *test* needs rebuild to be permitted,
  which is category 8. Either that test sits in 8 (code before test, as with
  v2's [18]/[19]), or category 8 moves earlier in the order. The dependency
  graph decides; do not guess it.

