# The Append-Only Aux Log — `vec-hnsw-aux-log`

*Why the auxiliary table was rewritten, what it bought, and what is still owed.*

This is the design document for the `vec-hnsw-aux-log` branch. It is a delta against
`hnsw-design.md`, which describes the one-row-per-node aux the other branch keeps; read that
first. "Phase 2" below means this branch's work.

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
followed — the first two operational, the third a silent correctness bug. They are
`hnsw-design.md` §4.2 and §4.1 respectively.

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

Before the list: two changes landed together here and are worth separating, because only one
of them needed the log.

**Capture-under-lock is about ordering, and could have shipped in Phase 1.** The fork now
keeps a per-element `element_versions_` counter and takes both the version and the neighbour
snapshot *inside* that node's `link_list_locks_` region, at each of the three sites that
mutate an edge list. That is what gives the disk any way to know which of two writes to the
same node happened later in memory. It is necessary for fixing P2 — and **not sufficient**:
capture correctly under the lock, then write with an in-place UPDATE, and the older snapshot
can still land last and win. Phase 1 could have fixed P2 on its own by adding a `ver` column
and making the write conditional (`... WHERE id = ? AND ver < ?`), so the stale update matches
no row. What that would *not* fix is P1, because the UPDATE still takes an X lock held to
COMMIT.

So: **the fork change buys ordering; the log buys lock-freedom.** They are independent, and
only the second one required a new aux layout.

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

### Which limitations this addresses

| `hnsw-design.md` | Phase 2 |
|---|---|
| §4.1 lost neighbour-list update | **fixed** — no overwrite is possible, and `ver` carries the order |
| §4.2 deadlocks and hub serialization | **fixed** — distinct-key appends, nothing held to COMMIT |
| §4.3 dangling edges | contained, not fixed — same absorber, smaller surface |
| §4.4 reload retry on concurrent commits | unchanged |
| §4.5 graph memory held for server lifetime | unchanged — and reload, when it does happen, is more expensive here (§3b) |
| §4.6 IMPORT leaves the index empty | unchanged |
| §4.7 MVCC check ② unimplementable | **fixed** — `_dead` keeps `row_ref` and carries the deleter's identity (§4) |

It also introduces one of its own: the aux grows with mutations and is only pruned at reload
(§6).

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

**Attribution, so the log is not over-credited:** the MVCC design is the *original*
design's, not this phase's — see §4.

## 4. What this changes for MVCC — less than it looks

The MVCC design is **not** a Phase 2 achievement, and earlier drafts of this document
implied it was. It belongs to the original design and is described there
(`hnsw-design.md` §3.9): labels are already versions, nodes are already retained, the hidden
`vec_idx_id` is already versioned by undo, and edges are already hints — so the shared cache,
the private per-transaction cache, the three-tier lookup, the prepare-time eviction and the
whole-cache invalidation were never needed. That conclusion required no change to the aux
table at all.

Of the two read-side checks that design needs, **check ① — `row.vec_idx_id == candidate
label` — is implementable on the original layout unchanged.** It is a base-row fetch the read
path already performs plus one integer comparison.

Phase 2's contribution is confined to **check ②**, view-gated tombstone inclusion, and it is
a narrow but real one. For an old reader to return a row deleted after its snapshot it must
fetch that row, which needs the label's `row_ref`. The original delete is
`row_ref = NULL` — it destroys precisely that value — and the loader then skips the row, so
the node is not even a candidate.

`_dead` fixes exactly that, and two details of it matter:

- the log row keeps its `row_ref`, so the value an old reader needs survives the delete;
- the deletion is a **row of its own**, so its hidden `DB_TRX_ID` is unambiguously the
  *deleter's* identity. That is not true of an in-place tombstone: a dead node can still have
  its edge list rewired (see §6), and that later UPDATE would overwrite the row's
  `DB_TRX_ID`, losing the delete's timestamp.

So the honest summary is: Phase 2 did not simplify MVCC — the original design already had the
insight. Phase 2 removed the one obstacle to implementing half of it, and it removed the
write-side locks that made the cache-eviction machinery look necessary in the first place.

**This is now the weakest claim in the document.** `_dead` solves check ② only because the aux
holds `row_ref` at all. If `row_ref` moves to a unique index on the base table's `vec_idx_id`
(`hnsw-design.md` §3.11), a delete-marked index entry answers check ② by itself, and `_dead`
loses its visibility role entirely — leaving it a garbage-collection device at most. §7 works
through what that would mean here.

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

---

## 7. If `row_ref` moves to a base-table index

**Conditional.** `hnsw-design.md` §3.11 on the `vec-hnsw-aux-original` branch proposes
dropping `row_ref` from the aux entirely and resolving `label → row` through a unique index on
the base table's hidden `vec_idx_id` — the `FTS_DOC_ID_INDEX` analogue. Nothing here is
decided or implemented; this section records what that decision would do to *this* branch,
because it turns out to remove most of §5's and §6's difficulties rather than add to them.

### 7.1 The format it enables

```
vec_hnsw_<tid>_<iid>:
  seq        BIGINT UNSIGNED PRIMARY KEY  -- monotonic, taken under the node's link lock
  label      BIGINT UNSIGNED
  vec        BLOB                         -- non-NULL only on a birth row
  level      TINYINT                      -- birth row only
  neighbors  BLOB
```

Against §5's `PK(seq)` proposal, four things go away:

- **`row_ref`** — the column, and with it the override row a PK-only UPDATE had to append. A
  PK change now touches nothing at all: InnoDB re-points the index entry itself.
- **The `_dead` table, completely.** It existed to record a deletion as an append rather than
  an in-place `row_ref = NULL`. With liveness owned by the base table's index, **a DELETE
  writes nothing to the aux.** §5's awkward second sequence `PK(dead_seq)` disappears with it.
- **The `row_ref` clause of the purgability rule** (§6). It becomes simply *garbage iff a
  higher-seq row exists for the same label* — no second condition to get wrong.
- **`ver`**, already folded into `seq` by §5.

So the log's entire write set is birth rows and neighbour snapshots, both appends of new keys
at the right edge. Nothing in the aux refers to the base table any more.

### 7.2 Reload becomes a replay, not a resolve

`seq` order is a **valid topological order**: a node must exist before it can be rewired, and
a rewired neighbour was inserted earlier, so a label's birth row always has a lower `seq` than
any of its snapshots. A single forward scan can therefore apply rows as it reads them — birth
row creates the node, each snapshot overwrites that node's edges, last writer wins — with no
`map<label, winner>` and no version resolution at all. Recovery-style replay, in `seq` order.

That removes the one cost §5's table charges to `PK(seq)`. The map becomes an *optimization*
(it avoids setting a hot node's edges repeatedly) rather than a requirement. The
implementation caveat is real though: today's loader hands a finished batch to `loadIndex`, so
incremental replay needs a different entry point into hnswlib.

### 7.3 Pruning is still needed — and splits cleanly in two

Removing `row_ref` does **not** fix log growth. The log still grows with mutations, ~N(1+M̄)
rows before compaction. But the garbage divides into two kinds that need completely different
reasoning, and only one of them involves read views at all.

**Kind A — superseded snapshots. No visibility reasoning whatsoever.**
Label L has snapshots at `seq` 100, 140, 200. Only 200 can ever matter. 100 and 140 are
garbage, and *no reader has an opinion about them*, because *edges are navigation, not data*
(`hnsw-design.md` §3.9): no query result depends on which edges exist, only on which labels
resolve to visible rows. So the rule is purely local — **garbage iff a higher `seq` exists for
the same label** — and it needs no read view, no transaction ids, and no probe. This is the
bulk of the garbage by volume.

**Kind B — whole labels.** A label's birth row plus its winning snapshot can go only when the
label is needed by nobody, ever again. This is the visibility-sensitive part, and it is what
§6 records as unimplemented ("reclaiming those needs a rule about the oldest active read
view").

### 7.4 How we know which labels to purge, without reasoning about views

The rule, and it is short:

> **Label L is prunable iff looking up `vec_idx_id = L` in the base table's index finds
> nothing at all — not even a delete-marked entry.**

Why that is sound, case by case:

| what the probe finds | meaning | decision |
|---|---|---|
| a live entry | some reader can see the row | keep |
| a delete-marked entry | purge has **not yet** removed it, and purge removes one only once no active read view can need it | keep (conservative) |
| nothing | purge is finished with L, and the *only* path from a label to a row is this index | **prune** |

The point is what we do *not* do: we never enumerate read views, never compare transaction
ids, never track the oldest view ourselves. We ask whether InnoDB's purge has already made
that decision. Purge is the component whose job is exactly "no view needs this any more", so
the correct move is to read its conclusion rather than recompute it.

Two properties make the probe safe rather than racy:

- **Labels are never reused** (`hnsw-design.md` §3.2). So a negative answer is *permanent* —
  once L is absent from the index it can never come back, and there is no window in which a
  new insert resurrects L between the probe and the delete.
- **Conservative is free.** A wrong "keep" costs space until the next pass. A wrong "prune"
  would lose a row an old reader is entitled to. The rule errs the safe way by construction,
  which is the right asymmetry for a GC rule.

Cost: one index dive per *candidate* label — only labels that already look dead, against a
small, hot index. Open implementation question: the probe needs to see delete-marked entries,
which is not an ordinary MVCC read; it wants purge's own see-all semantics.

### 7.5 Resuming after a restart

Three separate questions, and only the third needs a new mechanism.

**The `seq` counter.** Restore it as `max(seq)` over all rows. With `PK(seq)` that is the
rightmost record — one page, not a scan. (Strictly: max over *all* records including
delete-marked, the same conservative rule the label counter uses, so a `seq` consumed by a
rolled-back statement is never reissued.)

**Crash in the middle of compaction — nothing to repair.** Compaction is either row deletions
on a transaction or a rewrite-and-swap; both are atomic. A crash leaves the pre-state or the
post-state, never a mixture, and the graph is rebuilt from whatever survived. Correctness
needs no resume, and there is no partial-graph state to recover because the graph is never
persisted as such.

**Not redoing work already done — this is the real resume problem.** Without a marker, every
reload re-examines every row to rediscover the same garbage. A persisted **compaction
watermark** — *all `seq` < W are already compacted* — bounds each pass to `seq >= W`. The
device already exists in this codebase: the label counter persists its watermark through
`dict_table_vec_next_id_log` and `dict_table_persist_to_dd_table_buffer` (dynamic metadata),
and a re-minted aux (TRUNCATE, DROP/ADD INDEX) gets a new `table_id` and therefore a fresh
metadata row, so no invalidation logic is needed.

**The trap, which we have already met once.** Advancing W *before* the compaction's redo is in
the mtr is a crash window: crash after persisting W and the rows below it were never actually
compacted — and now never will be, because every future pass skips them. That is a permanent
unbounded leak rather than corruption, which makes it the kind of bug that is found late.
Either advance W in the **same mtr** as the deletions, or advance it only after commit and
accept re-scanning a little. This is the same shape as the autoinc persist crash window filed
separately; do not solve it by hand a second time.
