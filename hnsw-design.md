# HNSW Vector Index — Design

*Percona Server · InnoDB · `VECTOR KEY ... TYPE hnsw`*

This describes the vector index as originally designed: the in-memory graph, the single
auxiliary table that persists it, and how each statement maintains both.

Phase 2 replaced that auxiliary table with an append-only log and a companion `_dead`
table — see `hnsw-phase2-aux-log.md`. Read this document first; Phase 2 is stated as a
delta against it.

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
moves it, DISCARD/IMPORT TABLESPACE discards and re-mints it, ADD/DROP VECTOR INDEX creates
or drops just that index's table.

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
  virtual dberr_t recreate_after_import(dict_table_t*, trx) const = 0;
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
  dict_table_t        *table;      /* back pointer */
  const Vector_index  *impl;       /* who allocated this runtime */
  space_index_t        index_id;   /* the index it serves */
  uint16_t             field_no;   /* MySQL ordinal of the vector column */
  uint32_t             dims;
  virtual ~Vec_runtime() = default;
};
```

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
  has `KEY::vector_index_type`. They resolve with `vec_index_by_name()`. Since CREATE/ALTER
  already validated the token, it must resolve; the fallback is pure defence for a data
  dictionary written before the token existed.
- **No token, but a runtime is open** — everything else (insert, delete, knn, close, …) uses
  `vec_index_for(table)`, which reads `table->vec->impl`. **An open runtime is
  self-describing**: it remembers which implementation created it, so dispatch never re-reads
  the DD, and teardown from `dict_mem_table_free` is correct even for a type this code has
  never heard of.

The residual case is a table with no runtime open — teardown on a table that never built one,
and IMPORT re-mint before first open. Those are no-ops or type-independent for every type,
so answering "hnsw" is correct while it is the only registered type; a second type threads
its token through the IMPORT site.

Note what is *not* here: no per-index `vec_type` column in the dictionary, and no type stored
on `dict_index_t`. The type is either carried by the token from the SQL layer or implied by
the open runtime. That keeps one authority for each answer instead of two that can disagree.

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

---

## 4. What this deliberately does not do

- **No on-disk traversal.** Searching from disk is a different algorithm; this design keeps
  the graph resident and uses disk only to reconstruct it.
- **No SERIALIZABLE on the index path.** Phantom prevention needs predicate locks over "the
  k nearest neighbours of q", and there is no key order on ℝᵈ to hang gap locks from. READ
  COMMITTED and REPEATABLE READ work fully; the exact path remains for sessions needing more.
- **One vector index per table**, and a single-column integer PRIMARY KEY, for now.

---

*Next: `hnsw-phase2-aux-log.md` — why the §3.5 write pattern had to change, and what
replaced it.*
