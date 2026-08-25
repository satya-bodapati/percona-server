# HNSW Aux Storage — the sub-transaction design

*Percona Server · InnoDB · `VECTOR KEY … TYPE hnsw` · branch `vector-mvp-syntax-aux`*

This is the MVP design for persisting an HNSW index and serving `ORDER BY DISTANCE … LIMIT k`
from it. It is written against the upstream HNSW class (`vector-common/hnsw.h`, PS-11266 +
PS-11267) and adds only what that class deliberately leaves to its user: storage, identity,
and the transactional story.

Everything here follows from **two decisions**, and most of the design is their consequence:

1. **The aux table stores the base row's primary key** (`base_pk`), rather than the base table
   carrying an index on the hidden label column.
2. **Aux writes ride a sub-transaction that commits independently**, and are never rolled back
   when the user's statement rolls back.

---

## 1. What the base already provides

| | |
|---|---|
| `vector-common/hnsw.h` | the graph: `insert`, `k_nn_search`, streaming search, lazy per-node loading, a `Persistor` callback concept |
| PS-11264 (DD) | index options — `TYPE` and the `WITH (…)` construction params — stored in and restored from the data dictionary |
| PS-11203 (syntax) | `VECTOR KEY (v) TYPE hnsw WITH (M = 16, ef_construction = 200)`, validation, error codes |

The class holds **no state on disk and no opinion about storage**. It hands us callbacks and
expects us to answer them. It also stores no metadata: *"users must store dimensions, M and
distance kind alongside the graph"* — for us that is the DD, via the `WITH()` options.

---

## 2. What the user sees, and what lands on disk

Nothing new in the SQL surface — that is the base's. Two things appear that the user did not
declare:

**A hidden column on the base table.** `vec_idx_id BIGINT UNSIGNED`, invisible to `SELECT *`
and `SHOW COLUMNS`, holding this row's **label** — the graph's name for it. The FTS_DOC_ID
device. Unlike FTS, **there is no index on it.**

**One auxiliary table per vector index**, `vec_hnsw_<table_id>_<index_id>`, hidden from
`SHOW TABLES` and `INFORMATION_SCHEMA.TABLES`, visible in `INNODB_TABLES`. The `vec_` prefix
is reserved.

---

## 3. Specification

### 3.1 Terminology

| Term | Meaning |
|---|---|
| **label** | A graph node's identity, and the `id` the HNSW class takes. From a per-table counter; stored in the base row's hidden `vec_idx_id`. **Never reused. Never zero** — 0 is the class's empty-slot sentinel. |
| **base_pk** | The base row's PRIMARY KEY, stored on the node and in its aux row. Single-column `BIGINT UNSIGNED` (PS-11264 restricts it). Several nodes may share one. |
| **fresh label** | Changing a vector mints a **new** label rather than editing the old node. So a `(label, vector)` pair is immutable. |
| **orphan** | A node whose label no visible base row claims — from a rolled-back statement, a delete, or a superseded vector. Filtered at read time; never removed. |

### 3.2 The aux table

**One row per graph node, updated in place.**

```sql
CREATE TABLE vec_hnsw_<tid>_<iid> (
  id        BIGINT UNSIGNED PRIMARY KEY,  -- the label = HNSW node id
  base_pk   BIGINT UNSIGNED NOT NULL,     -- the base row this node describes
  vec       BLOB NOT NULL,                -- dims * 4 bytes
  level     TINYINT NOT NULL,
  neighbors BLOB NOT NULL                 -- node ids, 0 = empty slot
)
```

No version column, no sequence, no tombstone, no companion `_dead` table. A node's row is
written once and rewritten whenever its edges change.

`PRIMARY KEY(id)` is not incidental — it is what makes lazy loading possible. `load_node_cb`
is a point lookup by node id, so the label must lead the key. (An append-only `PK(seq)` log
would give sequential writes but turn every node fault into a scan.)

**Entry point.** `update_entry_point_cb` must be persisted somewhere, because cold start
begins with `init_from_entry_point()`. It is a single value per index — a reserved metadata
row in the aux, since label 0 is never a real node id.

### 3.3 The label counter

The class requires a non-zero `id` that does not already exist. We allocate from a per-table
counter, stamped into the base row's hidden column at INSERT.

**A label must never be reissued**, including after a crash that followed a rolled-back
statement — a reissued label would alias two different rows in one graph. The counter is
therefore persisted as a watermark ahead of what has been handed out, and restored at startup.

### 3.4 The persistor

We implement the class's `Persistor` concept. `Context` is ours to define, and it carries the
sub-transaction, the THD, and an error slot.

| callback | what we do |
|---|---|
| `insert_cb(ctx, id, base_pk, q, layer, neighbors)` | `INSERT INTO aux VALUES (id, base_pk, q, layer, neighbors)` |
| `update_neighbors_cb(ctx, id, neighbors)` | `UPDATE aux SET neighbors = ? WHERE id = ?` |
| `update_entry_point_cb(ctx, id)` | rewrite the metadata row |
| `load_node_cb(ctx, hnsw, handle)` | one PK lookup, then `load_set_layer` / `load_set_vec` / `load_set_base_pk` / `load_node_neighbors` |

**Errors.** The class ignores callback return values. We do not need an API change: the
callback records the failure in *our* `Context`, and the caller checks it after `insert()`
returns and fails the statement. Later callbacks in the same insert may still run; that is
harmless, because a failed statement rolls the base row back and whatever reached the aux
becomes an orphan.

**Concurrency.** The class has no locking of its own. Writers are therefore serialised by a
per-index latch held across `insert()`. That is a throughput ceiling, and it is also why no
version column is needed: **concurrent rewires of the same node cannot happen**, so the
stale-snapshot race that would demand one is structurally absent. When upstream adds thread
safety, this must be revisited.

### 3.5 The sub-transaction rule

Aux writes do **not** ride the user's transaction. They run on a sub-transaction that commits
on its own, and a user rollback leaves them in place.

This is not a convenience. The class has no delete, so on rollback we **cannot** remove the
node from memory. If the aux rolled back while memory kept the node, disk and memory would
disagree and a restart would silently change the graph. Not rolling back keeps them telling
the same story.

Everything else follows from one invariant:

> **The aux is a superset of committed base rows.**

Extra nodes are harmless — the read path filters them. A *missing* node is not: the row
exists, kNN never returns it, and nothing reports the gap. So the failure direction must
always be "extra", which pins two ordering rules:

- **the sub-transaction commits before the user transaction commits** — a crash in that window
  leaves an orphan, never a gap;
- **a node's own row commits no later than any neighbour blob referencing it** — `insert_cb`
  fires before `update_neighbors_cb`, so this holds naturally. It is what makes a dangling
  neighbour id impossible, and therefore why `load_node_cb` never has to report "gone".

What this buys, beyond rollback coherence: the M neighbour row locks are held for the duration
of the aux write instead of the user's transaction. Insert-insert deadlocks on shared
neighbours, and serialisation behind hot hub nodes, largely disappear — and the user's undo
does not carry M neighbour updates.

### 3.6 Write paths

| statement | graph | aux |
|---|---|---|
| **INSERT** | `insert(label, pk, vec, &ctx)` | 1 INSERT + 1 UPDATE per rewired neighbour |
| **UPDATE** (vector changed) | `insert(fresh_label, pk, new_vec, &ctx)` — the old node stays | same as INSERT, for the new label |
| **UPDATE** (PK only) | nothing | `UPDATE aux SET base_pk = ? WHERE id = <current label>` |
| **DELETE** | nothing | nothing |

A NULL vector is not indexed: no node, no aux row. It still consumes a label.

**DELETE writes nothing**, and that is not a shortcut. Check ② requires that a reader whose
snapshot predates the delete can still reach the row — so the node *must* stay. The class's
missing delete support and MVCC's requirement happen to coincide.

**PK-only UPDATE** touches only the row's *current* node. Stale nodes may keep the old
`base_pk`; the read path rejects them anyway.

### 3.7 Read path

```
hits = k_nn_search(q, k, ef_search, &ctx)      → (node_id, base_pk) pairs, closest first

per candidate:
   fetch the base row by base_pk, under this reader's own read view
   not visible?                      → skip
   row.vec_idx_id != node_id ?       → skip          ← check ①
   otherwise emit at the candidate's graph distance

short of k? widen and resume, excluding what was already returned
```

Two dives per candidate? No — **one**. `base_pk` comes straight from the graph, so the only
disk access is the clustered fetch every index pays to return a row.

**This requires the node id in search results**, which the upstream API does not yet return
(§4). Without it the comparison marked ① cannot be performed at all.

### 3.8 How MVCC works — nothing special

There is no vector-specific visibility machinery. The graph is a **versionless candidate
generator**; the base row decides. Four properties, each present for its own reason, add up
to a multi-version store:

| property | why it exists | what it also gives |
|---|---|---|
| fresh label per vector change | counter crash-safety | every historical vector is a distinct, immutable node — *a version* |
| nodes are never removed | the class has no delete | version history is retained |
| `vec_idx_id` is an ordinary column | it is the FTS_DOC_ID analogue | it is **versioned by undo**, so the row version a reader sees names the label that represents it |
| edges are navigation, not data | HNSW rewires freely | the path to a candidate never affects what may be returned |

So one shared graph serves every snapshot: it is a *superset* of every reader's node set, and
each reader filters it with its own read view. Two checks, and only the first is ours:

**① `row.vec_idx_id == node_id`.** One integer compare, on a row the read path already
fetched.

Why it is not optional — one `UPDATE` is enough:

```
row 7:  v=[1,0]  → label 10
UPDATE t SET v=[0,1] WHERE id=7   → label 20; node 10 remains

graph:  node 10 → [1,0], base_pk 7        (stale)
        node 20 → [0,1], base_pk 7        (current)
base:   row 7 now has vec_idx_id = 20

SELECT … ORDER BY DISTANCE(v,'[1,0]') LIMIT 1
   node 10 matches exactly → distance 0 → returns base_pk 7
   row 7's actual vector is [0,1] — far from the query
```

Without ①, row 7 is returned **at the wrong distance** and sorts ahead of rows that genuinely
are near the query. Not a duplicate, not a missing row: a wrongly ranked one, invisible to the
caller. With ①, `20 != 10` skips the stale node, and node 20 later returns row 7 at its true
distance.

① also covers PK reuse: `DELETE` row 7, then `INSERT` a new row that takes PK 7. The stale
node still points at 7, and existence proves nothing about identity — but `vec_idx_id` does.

**② include a deleted row for a reader whose snapshot predates the deletion.** Free. The node
is still in the graph, `base_pk` is untouched by DELETE, and the fetch under an old read view
returns the pre-delete version from undo — whose `vec_idx_id` still reads 10, so ① passes too.

Every case, in one table:

| situation | base fetch | ① | outcome |
|---|---|---|---|
| another transaction's uncommitted insert | not visible | — | skipped |
| deleted, deletion visible to me | not visible | — | skipped |
| deleted **after** my snapshot | visible via undo | passes | **returned — ②** |
| vector updated, stale node | visible | `20 != 10` | skipped |
| PK reused by a different row | visible | `50 != 10` | skipped |
| statement rolled back (orphan) | not found | — | skipped |

**Isolation.** Whatever the transaction already has. REPEATABLE READ asks one view for the
whole transaction; READ COMMITTED asks a fresh one per statement. Neither needs anything from
us — which is the real argument for this design over versioning the graph.

**Not reachable, by design:** SERIALIZABLE on the index path. Phantom prevention needs
predicate locks over "the k nearest neighbours of q", and ℝᵈ has no key order for gap locks to
hang from. Sessions needing it fall back to the exact path.

### 3.9 Load path

The graph is **not** loaded wholesale. Cold start reads dims/M/ef from the DD, constructs the
`HNSW`, reads the persisted entry point, and calls `init_from_entry_point()`. From there,
nodes fault in as searches and inserts touch them:

```c
void load_node_cb(Context *ctx, HNSW &h, LoadNodeHandle handle) {
  uint64_t id = h.load_node_id(handle);
  //  SELECT base_pk, vec, level, neighbors FROM aux WHERE id = ?   (one seek)
  h.load_set_layer(handle, level);
  h.load_set_vec(handle, vec);
  h.load_set_base_pk(handle, base_pk);
  h.load_node_neighbors(handle, ids);   // 0 = empty; stubs for unloaded neighbours
}
```

Consequences worth stating: there is no multi-second warm-up on first access, memory tracks
what is actually queried rather than table size, and **dict-cache eviction needs no special
handling** — a lost graph is refaulted node by node, so nothing has to pin the table.

---

## 4. What this needs from upstream

**One thing: `k_nn_search` and the streaming search must return the node id alongside
`base_pk`.**

Today they return `base_pk` only, so a caller cannot tell which node produced a hit — and ①
becomes impossible. §3.8 shows the failure: a single vector `UPDATE` silently corrupts result
ordering. This is a gap in the class's own terms, since its documentation already anticipates
that "after updating an indexed vector, a new graph node may share the same `base_pk`" without
saying how a caller distinguishes them.

If it is not available, the alternative is a unique index on the base table's `vec_idx_id` and
searching *by* label — which cannot lose the label because it is the lookup key. That costs a
second B-tree (~21 bytes/row, maintained on every insert and vector update) and a second dive
per candidate, since a secondary-index consistent read falls back to the clustered index
whenever the page's `max_trx_id` is not older than the view — which, on an index being
appended to constantly, is nearly always.

Not required, but on upstream's own todo list and relevant to us: thread safety (we serialise
until then), deletes (we need none), and error returns from callbacks (our `Context` covers
it).

---

## 5. Limitations and impacts

**Orphans are permanent, and the index grows with mutations rather than rows.** A rolled-back
statement, a deleted row and a superseded vector all leave a node that is never removed. There
is no GC in this design. A workload that updates vectors heavily, or rolls back often, grows
the graph and the aux without bound.

Reclaiming would mean deciding that no reader can ever need a label again — which purge
already determines for the base row, but with `base_pk` in the aux (and no index on
`vec_idx_id`) there is no clean way to ask that question, because PK reuse makes "does this PK
exist" the wrong test. This is the one place the index alternative would be easier.

**Aux state is not transactional with base data.** By design. The aux is durable state
converging on the base table, and the base table is the sole authority on what is real. Anyone
reading `SELECT COUNT(*)` from the aux and comparing it to the base table will find them
unequal, and that is correct behaviour, not corruption.

**Writers are serialised** by a per-index latch, until upstream adds thread safety.

**A failed aux write fails the statement.** The alternative — committing a base row whose node
never landed — violates the superset invariant in the unsafe direction.

**INSTANT ADD/DROP COLUMN, IMPORT and DISCARD** are refused on vector-indexed tables, and
`ADD VECTOR INDEX` is COPY-only; a vector index's TYPE cannot be changed in place.

---

## 6. What this deliberately does not do

- **No on-disk traversal.** Searching from disk is a different algorithm; the graph stays
  resident and disk is used only to reconstruct it, one node at a time.
- **No version or sequence column.** Not needed while writers are serialised; revisit when
  upstream threading lands.
- **No `_dead` table, no tombstones.** DELETE writes nothing.
- **No dict-cache pinning.** Lazy loading makes eviction cheap enough to ignore.
- **One vector index per table**, and a single-column integer PRIMARY KEY.
