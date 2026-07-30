# HNSW Vector Index — Design

*Percona Server · InnoDB · `VECTOR KEY ... TYPE hnsw`*

This describes the vector index as originally designed: the in-memory graph, the single
auxiliary table that persists it, and how each statement maintains both.

There is a second line of development that replaces that auxiliary table with an append-only
edge log and a companion `_dead` table. It lives on the `vec-hnsw-aux-log` branch, with its
own document (`vec-hnsw-aux-log.md`) stated as a delta against this one; that branch is
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

`row_ref` is the part of this shape that does not survive contact with MVCC: it makes a delete
destructive, because `NULL = tombstone` erases the only address an old reader had. It is also a
second copy of something the base row already holds — the base row knows its label, so an index
on that label would answer the same question. §3.10 sets out both points; §3.11 removes the
column altogether and indexes the hidden `vec_idx_id` on the base table instead.

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

Three of these rows are what §3.11 changes. The two tombstoning rows lose their aux write
entirely — a delete just delete-marks the base row's `vec_idx_id` index entry — and
`markDelete` is dropped so the node stays a candidate for readers whose snapshot predates the
delete. The PK-changed-only row disappears completely: InnoDB re-points the index entry
itself, so that statement stops touching the vector index. INSERT is unaffected.

Rollback needs no aux-specific work — undo restores the rows — plus an in-memory inverse
(`unmarkDelete`, or marking a rolled-back insert deleted), applied from a per-transaction
list of what the statement did to the graph.

### 3.7 How SELECT works

**Queries never traverse the graph on disk and never read a `neighbors` blob.** The aux table
exists for reload; a query touches exactly one of its columns — `row_ref`, to turn a label
into an address.

```
1. traverse the in-memory graph        → k' candidate labels, closest first   (no I/O)
2. per candidate: read row_ref from the aux, keyed by label               (1 dive)
3.                fetch the base row by row_ref, under the reader's view   (1 dive)
4. skip it if that row is invisible, gone, or its vec_idx_id != the candidate label
5. emit at the candidate's graph distance
6. short of k? widen (k×2) and resume, excluding what was already returned
```

Step 2 is a point lookup on the aux's primary key, which *is* the label. Deliberately not a
resident `label → row_ref` map: that would be one entry per indexed row, rebuilt by a full aux
scan on every load (§3.10). It runs after the graph latch is released — it reads a B-tree, and
no graph state is involved, so holding the latch across it would block writers for the
duration of the I/O.

Step 4 is what keeps results correct under MVCC without versioning the graph: the graph offers
candidates, the **base row decides**. The label-match compare rejects a candidate whose row
has since moved to a different label, and costs one integer comparison.

So a query pays two dives per candidate. §3.11 does not remove the dive — it moves it off the
aux and onto an index on the base table, which returns the aux to being read only at reload.

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

Historically this was expressed as **two read-side checks** we would perform:

```
① after fetching the base row:   row.vec_idx_id == candidate label ?
② include a deleted label in the search iff the reader's view predates
   the deletion.
```

That framing is now obsolete, and §3.9a explains why: with a unique index on `vec_idx_id`,
neither is a check we perform. Both are outcomes of one ordinary indexed lookup. The two are
kept here because they name the *requirements* precisely, and the design below has to be
judged against them.

### 3.9a MVCC with the `vec_idx_id` index — the design

*This is the design this branch (`vec-hnsw-aux-original-mvcc`) implements. It is not yet
implemented; §3.12 lists what must still be decided.*

#### The read path is one call, not two steps

A unique index on the base table's hidden label column turns `label → row` from something we
assemble into something InnoDB already does:

```sql
t ( id PK, v VECTOR(768), vec_idx_id BIGINT UNSIGNED NOT NULL /*hidden*/,
    UNIQUE KEY vec_idx_id_index (vec_idx_id) /*hidden*/ )
```

```
per candidate label L:
   prebuilt->index = vec_idx_id_index          -- same device as ha_innodb.cc:12470
   search tuple:    vec_idx_id = L
   row_search_for_mysql(buf, PAGE_CUR_GE, prebuilt, ROW_SEL_EXACT, 0)
        └─ InnoDB: secondary lookup → clustered fetch → version build → visibility
   DB_SUCCESS                        → emit the row at the candidate's graph distance
   DB_RECORD_NOT_FOUND / END_OF_INDEX → skip this candidate
```

One call performs both dives. We do not resolve a PK and then fetch it; we **fetch the row
whose `vec_idx_id` is L**, under this reader's own view, exactly as `SELECT … WHERE
vec_idx_id = 42` would. `row_search_mvcc` already does the secondary→clustered resolution
(`row_sel_get_clust_rec_for_mysql`) and the version building
(`row_vers_build_for_consistent_read`) for every secondary index in the server.

Consequences worth being explicit about: `vec_knn_hit_t` no longer carries a `row_ref` at all —
search returns labels and distances — and the handler stops building PK search tuples.

#### Why both requirements are then satisfied for free

| situation | what the indexed lookup does | requirement met |
|---|---|---|
| another transaction's uncommitted INSERT | entry not visible to this view | skipped |
| row deleted, deletion visible to me | entry delete-marked, deletion visible | skipped |
| row deleted **after** my snapshot | delete-marked entry, but visibility resolves through the clustered record and undo | row returned — **②** |
| vector updated: L retired, M minted | `(L,pk)` delete-marked, `(M,pk)` inserted; an old reader still resolves L, a new reader finds nothing for L and reaches the row under M | both correct — **①** |

Requirement ① was "compare the fetched row's `vec_idx_id` against the candidate label".
Searching *by* that label makes the comparison structural: there is no branch of ours that can
be wrong, and no third place where the mapping could disagree.

#### Why it must be this lookup, and not one we write

This is the load-bearing decision of the whole design, so it is worth stating why the obvious
alternatives are wrong.

- **The FTS precedent does not transfer.** `fts_doc_fetch_by_doc_id()` solves precisely this
  problem for `FTS_DOC_ID` — and it does so through the **InnoDB SQL interpreter**
  (`pars_sql`/`que_eval_sql`), which this project forbids: it serializes every call on the
  global `pars_mutex`, and this runs per candidate per query. So FTS validates the *shape*
  (a unique index on the hidden column) but not the *mechanism*.
- **Hand-rolling the lookup forfeits the point.** A private `btr_pcur` walk over the secondary
  index would have to re-implement delete-marked-entry handling, secondary→clustered
  resolution and consistent-read version building. Those three things *are* requirements ① and
  ②. Reimplementing them is not "using InnoDB's MVCC" — it is writing our own and hoping it
  matches.

Hence `row_search_mvcc` through `prebuilt`, which is the same path a plain indexed `SELECT`
takes. The correctness argument is then "this is what every secondary index in the server
does", which is the strongest form available.

#### What the graph contains, and what a DELETE does

A DELETE stops touching the aux table and stops calling `markDelete`. The node stays in the
graph as a candidate, and the index decides per reader whether it resolves. So:

- the graph holds **every label whose aux row still exists**, live or not
- dead labels consume candidate slots, so `k` must widen to compensate — the resumable search
  loop (§3.7) already handles candidates that resolve to nothing
- nothing in memory or in the aux records liveness any more; there is one place that knows,
  and it is the base table

#### Isolation levels come for free

Both requirements are evaluated by `row_search_mvcc` against `prebuilt`'s read view, so the
isolation level is whatever the transaction already has:

- **REPEATABLE READ** — one view per transaction. A row deleted after the snapshot keeps being
  returned by every query in the transaction, matching what a primary-key lookup returns from
  undo.
- **READ COMMITTED** — a fresh view per statement. The same query twice in one transaction may
  legitimately differ.

That READ COMMITTED needs *no extra machinery* remains a real argument for this design: the
versioned-graph approach would have needed per-statement cache invalidation, whereas here a
newer view simply resolves the same shared graph differently.

#### Check ①: why it was needed (retained for the reasoning)

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

Check ② is **not** satisfied by the aux format of §3.4, and cannot be — a delete there
destroys the very thing an old reader needs. §3.10 shows exactly how; §3.9a is the design that
removes the problem instead of patching it.

**Isolation targeted:** READ COMMITTED and REPEATABLE READ (§3.9a).
**Not reachable, by design:** SERIALIZABLE on the index path — phantom prevention needs
predicate locks over "the k nearest neighbours of q", and ℝᵈ has no key order for gap locks
to hang from. Sessions needing it fall back to the exact path.

### 3.10 MVCC problems with the current aux format

Two problems, and they are different in kind. The first makes check ② impossible. The second
is about what the design costs to run, and it is the one that decides the fix.

#### Problem 1: a delete destroys the address

Row `id=7, v=[1,0]` indexed under label 42.

| time | session A (REPEATABLE READ) | session B |
|---|---|---|
| t1 | `SELECT …` → read view **V_A** created; row 7 visible | |
| t2 | | `DELETE FROM t WHERE id = 7;` **COMMIT** |
| t3 | `SELECT id FROM t ORDER BY DISTANCE(v,[1,0]) LIMIT 1` | |

At t3 the correct answer is **`id = 7`**. A's snapshot predates the delete, and
`SELECT id FROM t WHERE id = 7` in that same transaction *does* still return 7 from undo. If
the index path disagrees, one transaction gets two different answers depending on which plan
the optimizer chose — a non-repeatable read introduced by an index.

It disagrees. At t2 the delete does `UPDATE aux SET row_ref = NULL WHERE id = 42` and marks
the graph node, so hnswlib will not offer label 42; and even if it did, the loader skips
tombstoned rows, so there is no `label → row_ref` entry to fetch with. **A gets no rows.**

Two independent things are missing, and naming both is what makes the fix obvious:

- **the address is destroyed** — returning row 7 requires `row_ref = pk 7`, and the delete
  overwrote that column. The old value survives only in the aux row's own undo, which is not
  reachable by label and is eventually purged.
- **there is nothing to judge with** — even if `row_ref` were kept, including a dead node is
  correct only for readers whose view predates the deletion. That needs the deleting
  transaction's identity, which this format records nowhere.

#### One way to fix it: a `del_trx_id` column

A delete stops destroying anything and starts recording:

```
vec_hnsw_<tid>_<iid>:
  id          BIGINT UNSIGNED  PK     -- the label
  vec         BLOB
  row_ref     VARBINARY(3072)         -- NEVER nulled any more
  level       TINYINT
  neighbors   BLOB
  del_trx_id  BIGINT UNSIGNED         -- 0 = live, else who deleted it
```

- **DELETE** becomes `UPDATE aux SET del_trx_id = <deleting trx> WHERE id = label`; `row_ref`
  is untouched and the graph node is *not* marked deleted.
- **The loader** keeps dead rows and builds a second map, `label → del_trx_id`.
- **Search** includes a dead candidate **iff `!view->changes_visible(del_trx_id)`** — this
  reader cannot see the deletion, so the row is still theirs.

It has to be an explicit column, not the aux row's `DB_TRX_ID`: a dead node's aux row keeps
being UPDATEd by unrelated rewires, and every rewire overwrites `DB_TRX_ID` with the rewiring
transaction, destroying the deletion timestamp. A dedicated column is written only by the
delete.

This works. It is not what §3.11 proposes, for the reason Problem 2 gives.

#### Problem 2: `row_ref` is a second copy of what the base row already knows

Search happens in the in-memory graph, which yields a **label**. A label is not an address, so
something has to answer `label → base PK`.

This was once a resident `unordered_map<uint64_t, std::string>` rebuilt from the aux's
`row_ref` column on every load — roughly 64–72 bytes per entry, so **~0.7 GB at 10M rows**,
held for as long as the table was cached. That was not a scalable answer and the map is gone;
the read path now does a point lookup on the aux, which is already keyed by label. Counting a
single candidate, where a *dive* is one root-to-leaf traversal:

| step | with the map (removed) | today |
|---|---|---|
| `label → PK` | 0 dives — RAM hash lookup | 1 dive — aux `PK(label)` |
| `PK → row` | 1 dive — clustered, `ROW_SEL_EXACT` | 1 dive |
| **total** | 1 dive **+ O(rows) memory** | **2 dives, no resident state** |

So the memory objection is already settled, and it is worth being clear that this is *not*
what §3.11 buys. What remains is smaller but more fundamental: **`row_ref` duplicates
information the base row already holds.** The base row knows its own label — that is what the
hidden `vec_idx_id` column is — so an index on that column answers `label → PK` directly, with
the same two dives, no duplicate, and no column for a delete to destroy.

That reframes Problem 1 too. `del_trx_id` (above) keeps the duplicate and adds machinery to
protect it. Removing the duplicate removes the problem instead: there is nothing for a delete
to erase, because the mapping lives where the deletion is already recorded.

### 3.11 The aux format it needs

Drop `row_ref` entirely, and index the hidden column instead — the `FTS_DOC_ID_INDEX`
analogue this design knowingly went without:

```sql
-- base table gains an index on the column it already has
t ( id PK, v VECTOR(768), vec_idx_id BIGINT UNSIGNED NOT NULL /*hidden*/,
    UNIQUE KEY vec_idx_id_index (vec_idx_id) /*hidden*/ )

-- aux keeps only graph geometry
CREATE TABLE vec_hnsw_<tid>_<iid> (
  id        BIGINT UNSIGNED PRIMARY KEY,  -- the label
  vec       BLOB NOT NULL,
  level     TINYINT NOT NULL,
  neighbors BLOB NOT NULL
)
```

The read path becomes, per candidate:

```
graph search                    → label 42
lookup vec_idx_id = 42 in vec_idx_id_index, UNDER THIS READER'S VIEW   (1 dive)
   not visible → skip this candidate
   visible     → yields pk 7
fetch clustered row 7                                                  (1 dive)
```

**The read cost is unchanged** — two dives, exactly as today (§3.7). This is worth stating
plainly, because it says what the change is and is not:

- **not** a memory win. That was won by deleting the resident map; the aux lookup already
  costs nothing resident.
- **not** a dive saved. The first dive moves from the aux to a base-table index — an index of
  ~16 bytes of payload per entry, about three levels deep at 10M rows, which pages like any
  other.

What it buys is everything below: both MVCC checks stop being ours to implement, an entire
write path disappears, and the aux stops referring to the base table at all. A query then
reads only the base table and its own index, so the aux goes back to being touched solely at
reload — the property §3.7 had while the map existed, now without the map.

#### 1. Why this is MVCC-friendly

Because the mapping is now an ordinary secondary index entry `(vec_idx_id, PK)`, it is
versioned by undo like any other. Every case the two checks existed to handle is handled by
machinery that already exists and is already tested:

| situation | what the index lookup does | correct? |
|---|---|---|
| another transaction's uncommitted INSERT | entry not visible | skipped ✓ |
| row deleted, deletion visible to me | entry delete-marked, deletion visible | skipped ✓ |
| row deleted **after my snapshot** | entry delete-marked, but visibility resolves through the clustered record and undo | **row returned ✓ — this is check ②** |
| vector updated: 42 retired, 44 minted | `(42,pk)` delete-marked, `(44,pk)` inserted. An old reader searching 42 still resolves it; a new reader finds nothing for 42 and reaches the row under 44 | **both correct ✓ — this is check ①** |

Check ① was "compare the fetched row's `vec_idx_id` against the candidate label". Searching
*by* the label, through an index on that column, makes the comparison structural instead of a
step we perform and could get wrong.

#### 2. Why `del_trx_id` is no longer needed

`del_trx_id` existed to answer one question: *may this reader see a node whose row was
deleted?* A delete-marked secondary index entry answers it already — that is precisely what
InnoDB keeps delete-marked entries readable for. So:

- no new column, and no second `label → del_trx_id` map
- no `!view->changes_visible(...)` filter of our own to write, test, and get right at every
  isolation level
- the deleting transaction's identity is where InnoDB already puts it, not in a column we
  maintain by hand and must protect from being clobbered by unrelated rewires

The `_dead` table of the Phase 2 branch loses its MVCC justification for the same reason; what
remains there is a garbage-collection role, not a visibility one.

#### 3. What else it makes better

- **The PK-only UPDATE path disappears entirely.** Secondary index entries contain the PK, so
  InnoDB re-points `(42 → pk)` itself. `vec_aux_update_row_ref`, `vec_refresh_row_ref` and one
  interface virtual all cease to exist. Today a PK change still writes the aux column — the
  last thing that write is for — and afterwards it does not touch the vector index at all.
- **No tombstone convention.** `row_ref = NULL` is gone, so the aux holds no representation of
  liveness to keep consistent, and a DELETE writes nothing to the aux at all.
- **The loader stops caring about liveness.** It feeds geometry to `loadIndex` and stops: no
  `row_ref` column to read, no NULL check, no dead-label list. Which rows a reader may use is
  decided per query at the base table, not filtered once at load time.
- **The aux becomes purely graph geometry**, which makes the separation the design claims
  literally true: the aux persists the *graph*, the base table owns the *label → row* mapping.
  Nothing in the aux refers to the base table any more.
- **A stale mapping cannot produce a wrong row.** Resolution goes through the base table's own
  index under the reader's view, so there is no cached copy that can disagree with it — which
  also removes the correctness burden from any future scheme that keeps graphs alive across
  dict-cache eviction (§4.6).

Costs, stated plainly:

- **One extra dive per candidate** (two instead of one), against a small hot index, next to a
  clustered dive already being paid.
- **Index maintenance on writes** — every INSERT and every vector UPDATE maintains it. Net-new
  write cost, partly offset by PK-only UPDATEs no longer writing the aux. FTS pays exactly
  this for `FTS_DOC_ID_INDEX`.
- **Aux purge needs a base-table probe.** With no liveness marker in the aux, deciding that an
  aux row is removable means asking whether any version with `vec_idx_id = L` still exists that
  a live reader could need. This is more work than reading a `del_trx_id`, and it is the one
  place this format is harder than the column.

### 3.12 Decisions still open

The design of §3.9a is settled in shape. These five are not, and each one changes code, so they
are listed rather than resolved by whoever implements first.

**1. Reclaiming aux rows for labels that are gone for good.** With liveness owned by the base
table, an aux row is removable only when no reader can ever resolve its label again. The rule
that follows from the design is short:

> label L is prunable iff a lookup of `vec_idx_id = L` finds **nothing at all** — not even a
> delete-marked entry

because purge removes a delete-marked entry only once no active view needs it, so its absence
*is* purge's own verdict. Two properties make it safe: labels are never reused, so a negative
answer is permanent and there is no probe/delete race; and the rule errs toward keeping, so a
mistake costs space rather than a row. **Open:** where the sweep runs (reload-time, as
collapse does today, or on demand), and how the probe sees delete-marked entries — that is not
an ordinary MVCC read.

**2. Do rolled-back INSERTs still need in-memory `markDelete`?** Today `ADDED` tracking marks
the node deleted on rollback. Under §3.9a the rolled-back row's index entry is gone, so the
label resolves to nothing and the candidate is skipped anyway — correctness no longer depends
on it. Keeping it stops a dead node occupying candidate slots until the next reload; dropping
it removes the last reason `vec_trx_op_type` exists. **Open:** which.

**3. What bounds dead-node accumulation.** Nothing calls `markDelete` for DELETEs any more, so
deleted nodes stay candidates and `k` widens to compensate. The resumable loop absorbs it, but
a table with a high delete rate degrades until something reclaims. **Open:** whether that is
acceptable for phase 1, and what the operator-visible signal is.

**4. Locking reads.** `SELECT … FOR UPDATE` reads the latest committed version, not the view's,
so a candidate whose vector changed since resolves to a row carrying a *different* label — and
the lookup correctly finds nothing for the old label. A locking `ORDER BY DISTANCE` could
therefore miss a row whose vector was updated. **Open:** refuse locking reads on the index
path, fall back to the exact path, or define the semantics deliberately.

**5. Whether the index is unconditional.** FTS creates `FTS_DOC_ID_INDEX` whenever the hidden
column exists. Doing the same is simpler and means the read path can assume the index; making
it conditional saves write cost on tables that never run a kNN query. **Open:** which, though
unconditional is the safer default and matches FTS.

---

## 4. Limitations and restrictions

Two different things, kept apart on purpose.

A **limitation** is a property of this design. Nothing refuses the operation; it just behaves
in a way you need to know about. §4.1–§4.5 are limitations. Phase 2
(`vec-hnsw-aux-log.md`) addresses §4.1–§4.3; §4.4 stands; §4.5 is fixed on this branch by
the aux format change of §3.11.

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
and so destroys the address the old reader needs (§3.10).

The consequence is a visible isolation gap: a REPEATABLE READ transaction that could still
fetch a row by primary key will *not* get it from a `ORDER BY DISTANCE` query if another
session deleted it after the snapshot. One transaction, two answers, depending on the plan.

The fix is designed (§3.9a): drop `row_ref` from the aux and index the hidden `vec_idx_id` on
the base table, so a candidate is resolved by one ordinary indexed lookup and both requirements
fall out of InnoDB's existing visibility machinery. Until it lands, this is the one isolation
property the index path does not honour.

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

*Next: `vec-hnsw-aux-log.md` — why the §3.5 write pattern had to change, and what
replaced it.*
