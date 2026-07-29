# HNSW Phase 2 — The Append-Only Aux Log

*Why the auxiliary table was rewritten, what it bought, and what is still owed.*

Read `hnsw-design.md` first. This is stated as a delta against it, and in particular against
its §3.4: **one `addPoint` produced one INSERT and M UPDATEs of shared rows, each holding an
X record lock until COMMIT.**

---

## 1. What we changed

**Before** (`hnsw-design.md` §3.3) — one row per graph node, its `neighbors` BLOB replaced in
place whenever an edge list changed:

```sql
PK(id)   id, vec, row_ref, level, neighbors        -- neighbors UPDATEd in place
                                                   -- row_ref = NULL means tombstone
```

**After** — one row per *mutation*, and deletes moved to their own table:

```sql
log:    PK(label, ver)   ver 0  = birth row  (vec, row_ref, level, neighbours)
                         ver >0 = a neighbours snapshot (vec/level NULL)

_dead:  PK(label)        one row per deleted label — NEW in this phase
```

`_dead` replaces the tombstone: there is no "deleted" flag to update, and no explicit
"deleted by" column either — the dead row's own hidden `DB_TRX_ID` is the deleter's
identity, so whether a delete is visible to a reader falls out of that reader's read view.

The write paths become, in full:

| Statement | Before | After |
|---|---|---|
| **INSERT** | 1 INSERT + M UPDATEs | 1 birth row + M snapshot rows — **all INSERTs** |
| **UPDATE** (vector) | tombstone UPDATE + INSERT + M UPDATEs | 1 `_dead` row + birth + M snapshots |
| **UPDATE** (PK only) | UPDATE `row_ref` in place | 1 override row: `(label, ver+1, row_ref = new PK, current neighbours)` |
| **DELETE** | UPDATE `row_ref` → NULL | 1 `_dead` row |

Nothing is ever UPDATEd. Every write, for every statement, is an INSERT.

The aux also became a **set** of tables rather than one, described by per-type descriptors,
so create/drop/TRUNCATE/RENAME/IMPORT iterate the set — which is why adding `_dead` needed no
changes to any DDL path.

## 2. Why

An INSERT rewires ~M existing nodes, so the original design issued M UPDATEs of shared rows,
each taking an X record lock **held until COMMIT** (`hnsw-design.md` §3.4). Three problems
followed — the first two operational, the third a silent correctness bug.

**Deadlocks.** Two sessions inserting similar vectors land in the same neighbourhood and
contend on the same rows. In opposite order, that is a textbook AB-BA cycle:

```
T1: X-lock n5 ✓ → wants n7 … blocked
T2: X-lock n7 ✓ → wants n5 … blocked        → deadlock; a victim rolls back
```

**Hub serialization.** High-degree nodes near the entry point are in nearly every
insert's write set. One hot row throttles the whole insert stream, and the lock is held for
as long as the transaction runs — seconds, in a multi-statement transaction.

**Silently lost edges — the quiet one.** The callbacks persisted a list *as captured at the
mutation*, but the order in which those writes reached disk was decided by row-lock
acquisition, which has no relationship to memory-mutation order:

```
memory:    n5=[a] → T1 adds v10 → [a,v10] → T2 adds v11 → [a,v10,v11]
captured:  S1=[a,v10] (T1)                  S2=[a,v10,v11] (T2)
disk:      T2 writes S2 and commits; T1 writes S1 afterwards
result:    the OLDER snapshot overwrote the newer — v11's incoming edge is gone on disk
```

Memory stayed correct, so nothing looked wrong until the next reload, when the edge simply
was not there. The X lock did its job perfectly; it just cannot know that S1 predates S2.

Locking harder cannot fix that last one. It needs an *ordering token* on disk.

---

## 3. What it bought

**Deadlocks became inexpressible.** Appends use distinct primary keys, so they take page
latches for microseconds and compatible insert-intention locks. There is no lock held to
COMMIT for another session to wait on, so no waits-for cycle can form.

**Overwrites became impossible.** Both snapshots exist as separate rows. `ver` carries the
mutation order, and the loader takes the highest visible version — regardless of which row
reached disk first, which transaction committed first, or whether one rolled back.

**Smaller undo, no read-modify-write.** The old UPDATE fetched the row, read the existing
neighbours BLOB (often off-page), and wrote a replacement, with an undo record carrying the
full BLOB before-image. An append writes a new row; its undo record is "remove this row".

**Rollback and crash got simpler to reason about.** The appended rows ride the user
transaction, so undo or recovery removes exactly the rows that statement wrote. There is no
partially-updated blob to reason about.

**One extension point, two tables.** `_dead` was added as a member of the index's aux set —
a descriptor entry. Create, drop, TRUNCATE, RENAME and IMPORT all iterate the set, so the
second table needed no changes to any of those paths.

The cost is stated plainly in §5.

---

## 3b. The trade, in one table

| | Old: row per node, UPDATEd | New: row per mutation, append-only |
|---|---|---|
| **Deadlocks between inserters** | real, hub-amplified | **inexpressible** — distinct keys, nothing held to COMMIT |
| **Lost edge on disk (P2)** | real, silent until reload | **impossible** — no overwrite; `ver` carries mutation order |
| **Write cost per rewire** | fetch row, read BLOB (often off-page), rewrite it; undo carries the full before-image | append a row; undo is "remove this row" |
| **Page placement** | M scattered pages | M scattered pages — *unchanged* until `PK(seq)` (§5) |
| **Aux size** | one row per node | one row per **mutation** — 8 rows can occupy 22 |
| **Reload cost** | scan N rows | scan raw rows: ~2.75× at `raw=22`, until collapse runs |
| **Space reclamation** | not needed | needed — and only at reload (§6) |
| **Loader complexity** | plain scan | group-by, version resolution, latest-non-NULL `row_ref`, collapse |
| **hnswlib fork delta** | callbacks | callbacks + `element_versions_` + capture under three link locks |
| **Dangling edges (P3/P4)** | tolerated at load | same tolerance, smaller surface — *not* eliminated |
| **Crash safety, trx atomicity, DDL** | — | unchanged |

**In one line:** we traded *space and reload cost* for *the elimination of a deadlock class
and a silent correctness bug*, and moved complexity out of the concurrent write path into the
single-threaded loader. Sequential I/O is reachable from this shape but not yet realised.

**Attribution, so the log is not over-credited:** the MVCC simplification in §4 comes mostly
from *labels-are-versions* plus base-row visibility, which does not depend on how the aux is
keyed. The log's contribution there is narrower: it makes `_dead` natural (turning the
tombstone map into a durable table rather than hand-fed RAM state), and it removes the
write-side locks that made the old plan's prepare-time cache eviction necessary at all.

## 4. How MVCC works now

**Yes — this removed the caches.** The earlier plan versioned *graph nodes*: a shared cache
holding only latest-committed nodes stamped with `db_trx_id`, a private per-transaction
cache for uncommitted changes and reconstructed older versions, a three-tier lookup, eviction
of touched nodes at prepare time under the still-held X locks, undo-chain walks to rebuild
old node versions, and whole-cache invalidation when a private cache overflowed.

None of that exists. What replaced it is the observation that the versions were already
there under a different name:

- a changed vector mints a **fresh label**, so every historical value is a distinct
  immutable node — that *is* a version;
- deleted nodes are **marked, not removed**, so history is retained;
- the base row's hidden `vec_idx_id` is an **ordinary column, versioned by undo** — so the
  row version a reader sees *names* the label that represents it;
- edges are **navigation, not data** — the path taken to a candidate never affects what may
  be returned, so nodes never needed versioning at all.

So the graph is a versionless candidate generator and InnoDB's existing read view is the
visibility oracle. Two small read-side checks finish the job:

```
① after fetching the base row:  row.vec_idx_id == candidate label ?
     one integer compare; rejects a candidate whose row has moved to another label
② include a dead label in the search iff the reader's view predates its deletion
     the _dead row's own DB_TRX_ID answers this
```

**One graph in memory, not one per snapshot.** Nothing is ever removed, so the single
current graph is a *superset* of every snapshot's node set. An old reader wants "today's
graph, filtered to what my view can see" — and that filter runs per candidate at fetch
time. Only the *edges* differ from the older reality, and edges decide the search path, not
what may be returned: worst case a slight recall difference, never a wrong row.

**Reached:** READ COMMITTED and REPEATABLE READ, in full.
**Not reachable, by design:** SERIALIZABLE on the index path — phantom prevention would
need predicate locks over "the k nearest neighbours of q", and ℝᵈ has no key order to hang
gap locks from. The exact path remains available for sessions that need it.

*Status: the two read-side checks are the phase-3 read path. The write side and the loader
described here are implemented; ① and ② land with it.*

---

## 5. Making it sequential

The append-only shape is **not** the same thing as sequential I/O, and it is worth being
explicit because the words invite the assumption.

With `PK(label, ver)`, an INSERT appends M snapshots whose labels are scattered across the
key space, so they land on **M scattered leaf pages** — the same page scatter the old
UPDATEs had. What the log bought was lock-freedom, ordering, and smaller undo; not
placement.

Sequential placement needs the primary key to be a **monotonic sequence**:

```
PK(seq)   seq, label, vec, row_ref, level, neighbours
          vec non-NULL marks the birth row
```

**One counter, and it must be taken under the node's link lock.** If `seq` were assigned
when the row is inserted, it would reflect arrival order rather than mutation order — the
lost-edge bug again, in new clothing. Taken under the lock, a label's `seq` values increase
in mutation order, which means `seq` is simultaneously the placement device and the ordering
token, and `ver` disappears.

| | `PK(label, ver)` (today) | `PK(seq)` |
|---|---|---|
| INSERT | M scattered leaf pages | **the right edge** — one mostly-dirty page, contiguous redo |
| DELETE | `_dead` PK(label): also scattered, since deletions arrive in arbitrary label order | `_dead` PK(dead_seq): also at the edge |
| Reload | grouping is free from PK adjacency | scan + `map<label, winner>`, latest wins |
| Collapse | losers are adjacent to winners | losers are the displaced map entries — no harder |
| Diagnostics | a label's history is a PK seek | needs a scan |

The reload map is one entry per label — the same order as the graph already in RAM, so it is
bookkeeping, not a new burden. Two traps to note: `_dead` keyed by `label` is **not**
sequential (labels are monotonic, but deletions are not ordered by label), and "fixing" the
diagnostic regression with a secondary index on `(label, seq)` would scatter that index's
writes and undo the entire point.

---

## 6. Gotchas

**Space is only reclaimed on reload — and there is no background collapser.**
This is the big one. The log grows with mutations, not rows: 8 base rows can occupy 22 log
rows. Superseded snapshots are delete-marked by the **loader** — the same scan
`hnsw-design.md` §3.7 describes, now resolving versions as it goes — on a system
transaction. That means reclamation happens when the graph is
(re)loaded: **server restart, dict-cache eviction, or a self-heal reload** — and *not* on a
long-running server that never evicts. There is no background thread doing this by design.

A worked example, from the test that pins it down:

```
insert 8 rows                        → count=8  raw=22   (8 births + 14 snapshots)
restart, then touch the table        → count=8  raw=12   (8 births + 4 winners)
restart again                        → count=8  raw=12   (idempotent)
```

`count=` is resolved nodes; `raw=` is rows the log actually holds. Note the *touch* — the
collapse runs inside the graph load, so inspecting the aux without opening the table shows
the pre-collapse log.

If unbounded growth on a long-lived server matters for a workload, the options are, in
order of cost: a periodic `ALTER TABLE ... DROP/ADD VECTOR INDEX` (re-mints a compact aux —
this is today's compaction story), then a scheduled background collapser (the same
operation, on a timer), which was deliberately left out of this phase.

**Dead labels are never collapsed away.** Collapse removes *superseded* snapshots. A
deleted label keeps its birth row and its winning snapshot indefinitely, and its `_dead`
row is never removed. Reclaiming those needs a rule about the oldest active read view, which
is not implemented — a rebuild is the only thing that clears them today.

**A collapse pass is skipped when the reload could not resolve everything.** If the scan saw
rows it could not make sense of under its read view (a concurrent writer mid-commit), the
whole pass is skipped: a reload that cannot see the full picture should not decide what is
garbage. It retries on the next reload.

**A `row_ref` override row is never collapsible.** A PK-only UPDATE appends a row carrying
the label's current base-row reference, and the loader takes the *latest visible non-NULL*
`row_ref`. Collapsing one because a later neighbours-only snapshot exists would silently
revert the label to its pre-move reference and point the index at a vacated PK. The
purgability rule is therefore: *garbage iff a committed higher version exists **and** the
row carries no `row_ref`.*

**Dead labels are not strictly frozen.** Construction filters deleted nodes out of its
candidate set, so a dead node is traversed as a router but not selected as a neighbour — its
list is normally never rewired again. The exception is the update/repair path when the entry
point itself is deleted: it can be selected, and a snapshot can appear for an
already-dead label. Do not build anything that assumes a dead label's last row is final.
