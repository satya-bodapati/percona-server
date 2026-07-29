# HNSW Vector Index — Design

*Percona Server · InnoDB · `VECTOR KEY ... TYPE hnsw`*

This describes the vector index as originally designed: the in-memory graph, the single
auxiliary table that persists it, and how each statement maintains both.

There is a second line of development that replaces that auxiliary table with an append-only
edge log and a companion `_dead` table. It lives on the `vec-hnsw-aux-log` branch, with its
own document (`hnsw-phase2-aux-log.md`) stated as a delta against this one; that branch is
referred to below as "Phase 2". This branch (`vec-hnsw-aux-original`) keeps the one-row-per-node
table and is where MVCC is being built — §3.9.

---

## 1. Goal

Give InnoDB an approximate nearest-neighbour index for `VECTOR` columns, so that

```sql
SELECT id FROM t ORDER BY DISTANCE(v, ?, 'EUCLIDEAN') LIMIT 10;
```

is answered from an index instead of a full scan, without giving up what InnoDB users
already assume: transactions, crash safety, DDL that works, and MVCC.

Two constraints shape everything below.

**The graph lives in memory.** HNSW is a navigable small-world graph; a search hops from
node to node in an order the query decides. No on-disk layout suits that access pattern, so
we do not traverse on disk — the graph stays in RAM and disk is used only to rebuild it.

**The engine owns durability.** The graph is a cache. Everything needed to reconstruct it is
written to an ordinary InnoDB table on the user's transaction, so a vector index is as
crash-safe as a secondary index and needs no separate recovery machinery. That table is
written for one purpose only — rebuilding the graph — and is never read by a query.

---

## 2. What the user sees, and what lands on disk

### SQL surface

```sql
CREATE TABLE t (
  id BIGINT UNSIGNED PRIMARY KEY,
  v  VECTOR(768),
  VECTOR KEY (v) TYPE hnsw WITH (M = 16, ef_construction = 200)
);
```

`TYPE` names the index implementation; `WITH (...)` carries its construction parameters.
Both round-trip through `SHOW CREATE TABLE`.

`TYPE` is validated at CREATE/ALTER against a registry of index implementations, so an
unknown type is rejected there rather than surfacing later as an engine error. hnsw is
currently the only registered type; §3.3 describes what a second one costs.

An index's `TYPE` is fixed for its life. Changing it means dropping the index and adding it
back, which rebuilds the graph — there is no in-place type conversion, because the aux
contents belong to the old implementation (§4.8).

Reads need no new syntax: `ORDER BY DISTANCE(...) LIMIT k` is recognised by the optimizer
and served by the index.

### On disk

Two things appear that the user did not declare.

**A hidden column on the base table.** `vec_idx_id BIGINT UNSIGNED`, invisible to
`SELECT *` and `SHOW COLUMNS`, holding this row's **label** — the graph's name for it. Same
device FTS uses with `FTS_DOC_ID`.

**One auxiliary table per vector index**, named `vec_hnsw_<table_id>_<index_id>`. Hidden
from `SHOW TABLES` and `INFORMATION_SCHEMA.TABLES`, visible in
`INFORMATION_SCHEMA.INNODB_TABLES` — again the FTS convention. The entire `vec_` prefix is
reserved: `CREATE TABLE vec_anything` is rejected.

Every DDL operation keeps the aux consistent: DROP and TRUNCATE drop or recycle it, RENAME
moves it, ADD/DROP VECTOR INDEX creates or drops just that index's table. DISCARD/IMPORT
TABLESPACE is refused rather than kept consistent — see §4.7 for why that is the better
answer.

---

## 3. Specification

### 3.1 Terminology

| Term | Meaning |
|---|---|
| **label** | A graph node's identity. A `BIGINT` from a per-table counter, stored in the base row's hidden `vec_idx_id`. |
| **row_ref** | The serialized PRIMARY KEY of the base row a label belongs to — how a search result becomes a row. |
| **neighbours** | A node's edge lists, one per HNSW level. The graph *is* these lists. |
| **level** | How many HNSW layers a node participates in; assigned randomly at insert, geometrically distributed (so >127 is impossible in practice). |
| **tombstone** | An aux row whose `row_ref` is SQL NULL: the label is no longer part of the loaded graph. |
| **fresh label** | Every change to a vector value mints a **new** label rather than reusing the old one. Labels are never reissued. |

### 3.2 Two facts you need before the rest makes sense

**Labels are never reused, and a changed vector gets a new label.** Update a row's vector and
the old label is retired, not edited. This exists for counter crash-safety — an id consumed
by a rolled-back statement must never be handed out again — and it means a `(label, vector)`
pair is immutable.

**HNSW deletion is unsafe, so nodes are never removed from the graph — only marked.**
Removing a node can disconnect the graph. A deleted node stays in memory as a router:
traversal may pass through it, searches never return it.

### 3.3 Supporting more than one index type

HNSW is one way to build a vector index; it will not be the only one. The engine is
therefore structured so that a second type is an addition rather than an edit — nothing in
the hnsw implementation, and no call site outside it, changes when one arrives.

Three pieces do that work.

**A type identity and a registry.** `Vec_index_type` is the enum; the registry is one static
table mapping it to a token and an implementation singleton:

```c
enum class Vec_index_type : uint8_t {
  HNSW = 0,
  /* a second TYPE (e.g. spann = 1) adds its value here, one registry
     row, and its implementation singleton — nothing else */
};

const Vec_type_entry vec_type_registry[] = {
    {"hnsw", &vec_hnsw_singleton},        /* order matches the enum */
};
```

The string token exists only at the two boundaries where SQL hands us text — DDL validation,
and reading `KEY::vector_index_type` back from the data dictionary at open or build. Past
those, the type travels as the enum. `vec_index_by_name()` is the case-insensitive boundary
lookup; `vec_index_by_enum()` is the O(1) internal one; `vec_index_token()` goes back the
other way, and is how the aux table name embeds its type (§3.4).

**An interface for the runtime operations.** `Vector_index` is an abstract class whose
implementations are *stateless singletons* — every per-index piece of state lives on the
table, so there is no lifetime to manage:

```c
class Vector_index {
  virtual Vec_index_type type() const = 0;

  virtual void    open(dict_table_t*, field_no, dims, M, ef) const = 0;
  virtual dberr_t load(dict_table_t*, THD*) const = 0;
  virtual dberr_t insert(trx, table, thd, label, vec, row_ref, len) const = 0;
  virtual dberr_t remove(trx, table, thd, label) const = 0;
  virtual dberr_t refresh_row_ref(trx, table, thd, label, row_ref, len) const = 0;
  virtual dberr_t knn(table, thd, query, dims, k, ef, hits, exclude) const = 0;
  virtual size_t  size_hint(const dict_table_t*) const = 0;
  virtual dberr_t build(trx, table, vec_index, dims, M, ef, thd) const = 0;
  virtual void    close(dict_table_t*) const = 0;
};
```

That list is deliberately exactly the operations that exist — the interface grew one method
at a time, added by the commit that first needed it, never speculatively. What stays
*outside* is just as deliberate: aux naming, the hidden `vec_idx_id` column, the label
counter and its persistence, the rollback plumbing, and the memory budget are all
type-independent machinery, so they are plain functions rather than virtuals.

**A generic runtime slot on the table.** `dict_table_t::vec` is the per-table companion —
the analogue of `dict_table_t::fts` — and it is typed as the *generic base*, not as hnsw's
struct:

```c
struct dict_table_t {
  ...
  struct Vec_runtime *vec;      /* nullptr until first open */
};

struct Vec_runtime {
  const Vector_index  *impl;       /* who allocated this runtime */
  space_index_t        index_id;   /* the index it serves */
  uint16_t             field_no;   /* MySQL ordinal of the vector column */
  uint32_t             dims;
  virtual ~Vec_runtime() = default;
};
```

There is deliberately no back pointer to the table. A runtime is reachable only *as*
`dict_table_t::vec`, so every caller already holds the table before it can reach the runtime;
storing it again would be a second copy to keep consistent across RENAME and rebuild, for no
reader that could not take a parameter instead.

`Vec_runtime` carries only the index's SQL-facing identity — the fields call sites legitimately
read. Everything hnsw-specific (the graph pointer, its rw-latch, `M`/`ef_construction`, the
loaded/stale flags, the label→`row_ref` map, memory accounting) lives in `vec_t`, which
derives from it:

```c
struct vec_t : public Vec_runtime {
  int M, ef_construction;
  hnswlib::HierarchicalNSW<float> *hnsw;
  ...
};
```

**Only the implementation that allocated a runtime may interpret its subtype.** That rule is
enforced by keeping the downcast file-local to the hnsw implementation:

```c
/* vec0aux.cc — this file called vec_open, so this file alone may downcast */
static inline vec_t *vec_hnsw(const dict_table_t *table) {
  return static_cast<vec_t *>(table->vec);
}
```

The compiler holds the line: a stray `table->vec->hnsw` elsewhere does not compile.

**How dispatch finds the right implementation.** Two paths, and the distinction matters:

- **A token is available** — `open` and `build` are reached from the SQL layer, which still
  has `KEY::vector_index_type`. They resolve with `vec_index_by_name()`. No runtime is open
  yet, so the token is not merely the convenient authority, it is the only one. An unresolved
  token is refused: `open` logs and leaves the index unopened, `build` fails the ALTER. It
  must not fall back to a guess — opening an index as the wrong type would build the wrong
  structure over the aux rows and return wrong neighbours instead of an error.
- **No token, but a runtime is open** — everything else (insert, delete, knn, close, …) uses
  `vec_index_for(table)`, which reads `table->vec->impl`. **An open runtime is
  self-describing**: it remembers which implementation created it, so dispatch never re-reads
  the DD, and teardown from `dict_mem_table_free` is correct even for a type this code has
  never heard of.

`vec_index_for()` returns `nullptr` when no runtime is open, and does not guess. That case is
reached only on teardown paths, where `nullptr` has an exact meaning — this table never opened
a runtime, so there is nothing to close. A resolver that guesses is worse than one that fails:
the failure is visible, and with a second type registered the guess silently hands one
implementation's table to another.

Note what is *not* here: no per-index type column in the dictionary, and no type stored on
`dict_index_t`. Three authorities already exist, each used exactly where it is the only thing
that can answer — the open runtime's `impl`, the KEY's TYPE token, and the aux table *name*,
which embeds the token for the DD and purge paths. A fourth would be a place to disagree.

A vector index's type never changes over its life. What makes that true is that TYPE is part
of the index definition for the purposes of ALTER comparison (§4.8) — a changed TYPE is a
changed index, served by a full rebuild, not a metadata edit.

### 3.4 The auxiliary table

**One table per vector index, one row per graph node, edited in place.** Its name embeds the
type token from §3.3 — `vec_hnsw_<tid>_<iid>` — so the datadir is self-describing and each
type owns a namespace. A node's row is
created when the node is created and modified whenever anything about that node changes —
its edge lists, or the base row it points at.

```sql
CREATE TABLE vec_hnsw_<tid>_<iid> (
  id        BIGINT UNSIGNED PRIMARY KEY,  -- the label
  vec       BLOB NOT NULL,                -- the vector, dims * 4 bytes
  row_ref   VARBINARY(3072),              -- base-row PK image; NULL = tombstone
  level     TINYINT NOT NULL,
  neighbors BLOB NOT NULL                 -- [nlevels][per level: count + labels]
)
```

`neighbors` is the interesting column: the node's edge lists, serialized. It has **exactly
one reader** — the loader (§3.8). Queries never touch it, which is why its shape is free to
change without affecting the read path (and in Phase 2, it does).

The table's identity is the label, so there is exactly one row per node at all times: no
history, no versions. Whatever a node's edges are *now* is what the row holds.

That `NULL = tombstone` convention is the one part of this shape that has to change: it makes
a delete destructive, which costs an isolation property. §3.9 replaces it with a `del_trx_id`
column — one row per node still, but a delete records *who* deleted the node instead of
erasing *where* its row was.

### 3.5 How the graph drives writes into the aux table

hnswlib calls back into InnoDB whenever it changes the graph, and those callbacks write
rows on the **user's transaction**.

The important consequence is that **one `addPoint` produces one INSERT and M UPDATEs.**
Inserting vector `v10` under label `L10`, landing next to existing nodes `n5` and `n7`:

```
in memory                          on disk (same user transaction)
──────────────────────────────     ────────────────────────────────────────────────
addPoint(v10, L10)
  L10's own lists are built    →   INSERT (L10, v10, row_ref, level, neighbours)
  n5's list gains L10          →   UPDATE  n5  SET neighbors = <n5's new list>
  n7's list gains L10          →   UPDATE  n7  SET neighbors = <n7's new list>
```

Each of those UPDATEs is a read-modify-write of a shared row: fetch the row, read the
existing `neighbors` BLOB, write a replacement. And each takes an **X record lock, held until
COMMIT** — because that is what an UPDATE does.

Two properties of that pattern are worth holding onto, because they are what Phase 2
addresses: the UPDATEs are on rows **shared with other concurrent inserters**, and the lock
on each is held for the **whole transaction**, not the duration of the write.

### 3.6 The write paths

| Statement | In memory | On disk |
|---|---|---|
| **INSERT** | `addPoint` | 1 INSERT (the new node) + 1 UPDATE per rewired neighbour |
| **UPDATE** (vector changed) | `markDelete(old label)`, `addPoint(fresh label)` | tombstone the old row (`row_ref = NULL`) + a full INSERT-and-UPDATEs set for the new label |
| **UPDATE** (PK changed only) | nothing — the node is untouched | UPDATE that row's `row_ref` to the new PK image |
| **DELETE** | `markDelete` | UPDATE that row's `row_ref` to NULL (tombstone) |

A NULL vector is not indexed at all: no graph node, no aux row. It still consumes a label.

The two tombstoning rows are what §3.9 changes: `row_ref = NULL` becomes
`del_trx_id = <deleting trx>`, and `markDelete` is dropped so the node stays a candidate for
readers whose snapshot predates the delete. Everything else in this table is unaffected.

Rollback needs no aux-specific work — undo restores the rows — plus an in-memory inverse
(`unmarkDelete`, or marking a rolled-back insert deleted), applied from a per-transaction
list of what the statement did to the graph.

### 3.7 How SELECT works

**Queries never read the aux table.** It is storage and reload only.

```
1. traverse the in-memory graph          → k' candidate labels, closest first   (no I/O)
2. per candidate: fetch the base row by row_ref, under the reader's read view
3. skip it if that row is invisible, gone, or its vec_idx_id != the candidate label
4. emit at the candidate's graph distance
5. short of k? widen (k×2) and resume, excluding what was already returned
```

Step 3 is what keeps results correct under MVCC without versioning the graph: the graph
offers candidates, the **base row decides**. The label-match compare rejects a candidate
whose row has since moved to a different label, and costs one integer comparison.

The only disk I/O a query performs is the base-row fetch in step 2 — the same lookup any
secondary index pays to return a row.

### 3.8 How reload works

The graph is rebuilt from the aux table on first access after startup, after a dict-cache
eviction, or as a self-heal when in-memory state was lost. One scan under a read view:

```
scan the aux clustered index:
  skip rows not visible to this view          (uncommitted / rolled back)
  skip rows whose row_ref is NULL             (tombstones — not part of the graph)
  emit (label, level, vector, neighbour lists)

loadIndex(rows)  → reconstructs the graph directly, no re-insertion,
                   so the rebuilt graph is byte-identical to the persisted one

also: the label counter is re-seeded to max(label) over ALL rows — visible or not,
      tombstoned included — so a committed label can never be reissued
```

**Dangling edges are tolerated.** A committed `neighbors` blob may name a label whose own row
no longer exists — its inserting transaction rolled back after the blob was captured. The
loader drops such edges: the graph stays connected, searches stay correct, and the cost is a
slightly less well-connected node.

### 3.9 How MVCC works

A vector index has to obey the same isolation rules as any other index: a REPEATABLE READ
transaction must keep seeing the rows its snapshot is entitled to, including rows another
session has since changed or deleted, and must not see rows created after it started.

The obvious way to get that — and the way this was first planned — is to version the graph:
a shared cache holding only latest-committed nodes stamped with `db_trx_id`, a private
per-transaction cache for a transaction's own uncommitted changes plus older node versions
rebuilt from undo, a three-tier lookup across them, eviction of touched nodes at prepare
time, and whole-cache invalidation when a private cache overflows.

**None of that is necessary, because the versions already exist.** Four properties of the
design above, each present for its own unrelated reason, add up to a multi-version store:

| Property | Why it exists | What it also gives us |
|---|---|---|
| fresh label per vector change (§3.2) | counter crash-safety | every historical vector value is a distinct, immutable node — *a version* |
| nodes marked, never removed (§3.2) | HNSW deletion is unsafe | version history is retained |
| `vec_idx_id` is an ordinary hidden column | it is the FTS_DOC_ID analogue | it is **versioned by undo**, so the row version a reader sees *names* the label that represents it |
| edges are navigation, not data | HNSW rewires freely | the path taken to a candidate never affects what may be returned, so nodes never needed versioning |

So the graph is a **versionless candidate generator** and InnoDB's existing read view is the
visibility oracle. One shared graph serves every snapshot, because nothing is ever removed:
the current graph is a *superset* of every snapshot's node set, and each reader filters it.

Two read-side checks are all that is required:

```
① after fetching the base row:   row.vec_idx_id == candidate label ?
     one integer compare. Rejects a candidate whose row has since moved to
     a different label — e.g. an UPDATE minted a fresh label and this
     candidate is the retired one, or vice versa.

② include a deleted label in the search iff the reader's view predates
   the deletion — so an old reader can still return a row deleted after
   its snapshot.
```

#### Check ①: already possible, and why it is needed

Check ① needs nothing from the aux table — it is the base-row fetch the read path (§3.7)
already performs, plus one comparison. Concretely, with row `id=7` indexed under label 42:

```
UPDATE t SET v = [0,1] WHERE id = 7;    -- mints label 99, retires 42

aux:   42 → vec=[1,0], row_ref=pk 7     -- the old value, still a node
       99 → vec=[0,1], row_ref=pk 7     -- the new one
base:  row 7 now carries vec_idx_id = 99
```

A reader searching near `[1,0]` still gets label 42 as a candidate — nothing removed it — and
following its `row_ref` fetches the *current* row 7, whose vector is `[0,1]`. Returning it
would answer "nearest to `[1,0]`" with a row that is not. Check ① rejects it: `99 != 42`. A
reader whose snapshot still shows `vec_idx_id = 42` passes the same check and correctly gets
the row. One integer compare, and the label does all the version bookkeeping.

#### Check ②: what breaks without it

Same table, row `id=7, v=[1,0]` under label 42:

| time | session A (REPEATABLE READ) | session B |
|---|---|---|
| t1 | `SELECT …` → read view **V_A** created; row 7 visible | |
| t2 | | `DELETE FROM t WHERE id = 7;` **COMMIT** |
| t3 | `SELECT id FROM t ORDER BY DISTANCE(v,[1,0]) LIMIT 1` | |

At t3 the correct answer is **`id = 7`**. A's snapshot predates the delete, and
`SELECT id FROM t WHERE id = 7` in that same transaction *does* still return 7 from undo. If
the index path disagrees, one transaction gets different answers depending on which plan the
optimizer chose — a non-repeatable read introduced by an index.

It disagrees today. At t2 the delete sets `row_ref = NULL` and marks the graph node, so
hnswlib will not return label 42; and even if it did, the loader skipped the tombstoned row,
so there is no `label → row_ref` entry to fetch with. **A gets no rows.**

Two separate things are missing, and naming both is what makes the fix obvious:

- **the address is destroyed** — returning row 7 requires `row_ref = pk 7`, and the delete
  overwrote that column. It survives only in the aux row's own undo, which is not reachable
  by label and is eventually purged.
- **there is nothing to judge with** — even with `row_ref` kept, including a dead node is
  correct only for readers whose view predates the deletion. That needs the deleting
  transaction's identity, which is recorded nowhere.

#### Check ②: one column fixes it

*Planned, not yet implemented.* A delete stops destroying anything and starts recording:

```
vec_hnsw_<tid>_<iid>:
  id          BIGINT UNSIGNED  PK     -- the label
  vec         BLOB
  row_ref     VARBINARY(3072)         -- NEVER nulled any more
  level       TINYINT
  neighbors   BLOB
  del_trx_id  BIGINT UNSIGNED         -- NEW: 0 = live, else who deleted it
```

Three changes follow, and nothing else moves:

- **DELETE** becomes `UPDATE aux SET del_trx_id = <deleting trx> WHERE id = label`. `row_ref`
  is untouched, and the graph node is *not* marked deleted.
- **The loader** keeps dead rows and builds a second map, `label → del_trx_id`.
- **Search** filters per candidate: `del_trx_id == 0` → live, apply check ①; otherwise include
  it **iff `!view->changes_visible(del_trx_id)`** — this reader cannot see the deletion, so the
  row is still theirs.

**Why an explicit column and not the aux row's `DB_TRX_ID`.** A dead node's aux row keeps
being UPDATEd by unrelated rewires — a neighbour list changing has nothing to do with the
delete — and every rewire overwrites `DB_TRX_ID` with the rewiring transaction's id,
destroying the deletion timestamp. A dedicated column is written only by the delete, so it
cannot be clobbered. This is the whole reason the column exists rather than reusing what
InnoDB already stores.

Two costs worth stating plainly rather than discovering later:

- **Dead nodes stay searchable.** They occupy candidate slots, so `k` has to widen to
  compensate — a recall and latency cost. The resumable-search loop (§3.7) already copes with
  candidates being filtered out, so the mechanism exists, but the cost is real. Phase 2's
  `_dead` table has exactly the same property; it buys the same thing with an insert instead
  of an update.
- **Purge needs a rule.** A dead aux row may be removed only once no live read view could
  still need it *and* the base row's own undo has been purged — otherwise a reader entitled to
  the row loses it. Reload-time collapse is the natural place for that sweep.

#### Isolation levels

Both checks are evaluated against InnoDB's read view, so the isolation level is decided
entirely by *which view is asked* — existing engine behaviour, nothing this design adds:

- **REPEATABLE READ** — one view per transaction, created at first read. A row deleted after
  the snapshot keeps being returned by every query in the transaction, matching what the exact
  path returns from undo.
- **READ COMMITTED** — a fresh view per statement. The same query twice in one transaction may
  legitimately differ: the first statement includes the dead node, the second excludes it.

That READ COMMITTED needs *no extra machinery* is a real argument for this design. The
versioned-graph approach would have needed per-statement cache invalidation to get it right;
here a newer view simply filters the same shared graph differently.

**Open question — locking reads.** `SELECT … FOR UPDATE` reads the latest committed version,
not the view's. Check ① compares the fetched row's `vec_idx_id` against the candidate label,
so if the vector changed since, the latest row carries a *new* label and ① legitimately fails
— meaning `SELECT … FOR UPDATE … ORDER BY DISTANCE` could miss a row whose vector was
updated. This needs a decision, not an assumption, before locking reads are supported on the
index path.

**Isolation targeted:** READ COMMITTED and REPEATABLE READ. Reached today only for inserts
and updates — deletes need check ②, hence the column above (§4.5).
**Not reachable, by design:** SERIALIZABLE on the index path — phantom prevention needs
predicate locks over "the k nearest neighbours of q", and ℝᵈ has no key order for gap locks
to hang from. Sessions needing it fall back to the exact path.

---

## 4. Limitations and restrictions

Two different things, kept apart on purpose.

A **limitation** is a property of this design. Nothing refuses the operation; it just behaves
in a way you need to know about. §4.1–§4.5 are limitations. Phase 2
(`hnsw-phase2-aux-log.md`) addresses §4.1–§4.3; §4.4 stands; §4.5 is fixed on this branch by
the `del_trx_id` column of §3.9.

A **restriction** is an operation deliberately *refused*, because every way of allowing it in
phase 1 was worse than not offering it. §4.6–§4.8 are restrictions. Each states what is
refused, why, and what lifting it needs — the shape of the fix is part of the decision, not
something to work out later.

### 4.1 A neighbour-list update can be lost on disk

The callbacks persist a node's edge list *as captured at the in-memory mutation*, but the
order those writes reach disk is decided by row-lock acquisition, which has no relationship to
mutation order:

```
memory:    n5=[a] → T1 adds v10 → [a,v10] → T2 adds v11 → [a,v10,v11]
captured:  S1=[a,v10] (T1)                  S2=[a,v10,v11] (T2)
disk:      T2 wins the row lock, writes S2, commits;  T1 writes S1 after
result:    the OLDER list overwrote the newer — v11's incoming edge is gone ON DISK
```

Memory stays correct, so nothing looks wrong until the next reload, where the edge is simply
absent. The effect is **silent recall erosion**, not a wrong answer or a crash. The X lock
serializes the writes perfectly; it just cannot know that S1 predates S2. Fixing this needs an
ordering token on disk, which this design has no field for.

### 4.2 Concurrent inserters deadlock, and hub nodes serialize

An INSERT UPDATEs ~M shared rows and holds each X lock to COMMIT (§3.5). Two sessions
inserting similar vectors land in the same neighbourhood; in opposite order that is an AB-BA
cycle and one is rolled back as the victim. Even without a cycle, high-degree nodes near the
entry point are in nearly every insert's write set, so one hot row throttles the insert
stream — for the duration of the transaction, not of the write.

Deadlocks are retryable, so this is a throughput limitation rather than a correctness one.

### 4.3 Dangling edges after rollback interleaving

A committed `neighbors` blob can name a label whose own row no longer exists, because the
blob was captured before that label's inserting transaction rolled back. The loader drops
such edges (§3.8): the graph stays connected and searches stay correct, at the cost of one
edge's worth of reachability per occurrence.

### 4.4 Reload can miss concurrently-committing rows, and retries

The loader scans under a read view, so it cannot see aux rows written by a transaction that
has not committed yet — even though that transaction's `addPoint` already happened in memory
and will commit moments later. The rebuilt graph is then missing a node that is committed on
disk. This is detected (the scan notes that it skipped invisible rows) and handled by marking
the runtime stale so a later access reloads again, converging after the writers commit. It is
correct but it is a retry loop, and a graph can be briefly incomplete.

### 4.5 A row deleted after a reader's snapshot is lost to the index path

Of the two read-side checks §3.9 needs, the label-match compare (①) works on this design as it
stands. View-gated tombstone inclusion (②) does not, because a delete sets `row_ref = NULL`
and so destroys the address the old reader needs.

The consequence is a visible isolation gap: a REPEATABLE READ transaction that could still
fetch a row by primary key will *not* get it from a `ORDER BY DISTANCE` query if another
session deleted it after the snapshot. One transaction, two answers, depending on the plan.

The fix is decided and scoped — a `del_trx_id` column on the aux table, so a delete records
who did it instead of erasing where the row was (§3.9). Until it lands, this is the one
isolation property the index path does not honour.

---

### 4.6 Restriction: a vector-indexed table is pinned in the dict cache

The graph hangs off `dict_table_t`. An LRU eviction of the dict entry frees it, so the graph
would be discarded silently, and the eviction path cannot rebuild it — that needs a
transaction and a read view, taken under the `dict_sys` mutex, while concurrent writers are
mutating the graph. **A table with a vector index is therefore never evictable.**

The pin follows the vector *index*, not the retained hidden column: a table that DROPs its
vector index becomes evictable again even though `vec_idx_id` survives.

The consequence is the cost: once such a table is opened, its graph is resident until the
server restarts. `innodb_hnsw_max_memory` bounds how much graph a server will *build*, but
nothing reclaims an idle one, and pinning grows the non-LRU portion of the dict cache.

Pinning removes only the *involuntary* path. Every DDL case frees the table object explicitly
— DROP TABLE, DROP INDEX, TRUNCATE, ALTER rebuild — so the graph is still released there.

**What lifting it needs.** The real problem is that graph lifetime is coupled to dict-entry
lifetime; pinning masks that rather than fixing it. The graph is the durable asset and the
companion is derived state, so the fix runs in that direction: a registry keyed by
`(table_id, index_id)` holding the graph and the label→`row_ref` map — the only two things
that cost a full aux scan to rebuild — with everything else in the companion recreated on
attach. Deferred deliberately: it trades nested ownership's *guarantee* of cleanup for five
invalidation sites that must each remember, and leaking a multi-gigabyte graph is a worse
failure than rebuilding one.

### 4.7 Restriction: DISCARD / IMPORT TABLESPACE is refused

An imported tablespace carries base rows that no aux table describes. Both ways of allowing
that were tried and both were worse than refusing:

- **Re-mint an empty aux.** The table works, but every imported row is invisible to kNN until
  the user happens to run DROP INDEX + ADD INDEX. A warning is not a guarantee anyone reads
  it — a silently incomplete index is exactly the failure this design is built to avoid. It
  also needed a counter re-seed from a clustered scan, and left the vector stub index flagged
  corrupt by `row_import`'s root check: two warnings the tests had to whitelist to stay green.
- **Rebuild during IMPORT.** Work the user did not ask for, inside a statement that is
  supposed to be metadata-only.

Refusing is the honest state: the aux and the base rows can never disagree. The check sits at
the entry of `discard_or_import_tablespace`, before the table lock, so a refused statement
leaves the table completely untouched. It is scoped to vector-indexed tables — a plain table
in the same server still round-trips normally.

**What lifting it needs.** The same decoupling as §4.6: a graph rebuildable from the clustered
index independently of the dict object. Given that, IMPORT can rebuild honestly instead of
producing an empty index.

### 4.8 Restriction: a vector index cannot be redefined in place

`ALTER TABLE t DROP KEY vk, ADD VECTOR KEY vk (v) TYPE … WITH (…)` is served by **COPY**, not
INPLACE. With `ALGORITHM=INPLACE` spelled out it is refused.

Why it cannot be INPLACE: the new index needs its aux table while the old one still holds a
name keyed by the same `(table_id, index_id)`, and the graph would have to be rebuilt with
different parameters under a live index. COPY handles it organically — every row goes through
the normal INSERT stamping path into a fresh table.

There is a subtlety worth stating, because it caused a real bug. A single-statement DROP + ADD
of the same index name does not reach the engine as a drop followed by an add:
`fill_alter_inplace_info` matches old and new keys by *name*, so both halves collapse into one
key that is either "modified" or "unchanged". `has_index_def_changed()` decides which, and it
compares algorithm, key flags and comment — none of which hold a vector key's TYPE or WITH()
parameters. So a changed TYPE or `M` compared as **unchanged** and the whole statement was
skipped while reporting success. Both are now part of the comparison (TYPE
case-insensitively, since it is an identifier).

This is also what keeps a type immutable in practice: a TYPE change is a changed index served
by a full rebuild, never a metadata edit that would leave `SHOW CREATE` describing a structure
the aux does not contain. The first ADD after a plain DROP INDEX is unaffected and still runs
INPLACE — that table has no vector index at the time, so the refusal does not apply.

---

## 5. What this deliberately does not do

- **No on-disk traversal.** Searching from disk is a different algorithm; this design keeps
  the graph resident and uses disk only to reconstruct it.
- **One vector index per table**, and a single-column integer PRIMARY KEY, for now.

---

*Next: `hnsw-phase2-aux-log.md` — why the §3.5 write pattern had to change, and what
replaced it.*
