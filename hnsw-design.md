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
crash-safe as a secondary index and needs no separate recovery machinery.

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

`TYPE` is resolved against a **registry** of index implementations, and validated at
CREATE/ALTER — an unknown type is rejected there rather than surfacing later as an engine
error. At runtime the engine dispatches through a `Vector_index` interface, so adding a
second index type is one enum value, one registry row, and one implementation; no call site
changes. hnsw is currently the only registered type.

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

### 3.3 The auxiliary table

One row per graph node:

```sql
CREATE TABLE vec_hnsw_<tid>_<iid> (
  id        BIGINT UNSIGNED PRIMARY KEY,  -- the label
  vec       BLOB NOT NULL,                -- the vector, dims * 4 bytes
  row_ref   VARBINARY(3072),              -- base-row PK image; NULL = tombstone
  level     TINYINT NOT NULL,
  neighbors BLOB NOT NULL                 -- [nlevels][per level: count + labels]
)
```

`neighbors` is the interesting column: it is the node's edge lists, serialized. It has
**exactly one reader** — the loader (§3.7). Queries never touch it.

### 3.4 How the graph drives writes into the aux table

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

That property is the subject of Phase 2.

### 3.5 The write paths

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

### 3.6 How SELECT works

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

### 3.7 How reload works

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
  the graph resident and treats disk as a rebuild log.
- **No SERIALIZABLE on the index path.** Phantom prevention needs predicate locks over "the
  k nearest neighbours of q", and there is no key order on ℝᵈ to hang gap locks from. READ
  COMMITTED and REPEATABLE READ work fully; the exact path remains for sessions needing more.
- **One vector index per table**, and a single-column integer PRIMARY KEY, for now.

---

*Next: `hnsw-phase2-aux-log.md` — why the §3.4 write pattern had to change, and what
replaced it.*
