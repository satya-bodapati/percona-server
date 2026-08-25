# HNSW Aux Storage — the sub-transaction design

*Percona Server · InnoDB · `VECTOR KEY … TYPE hnsw` · branch `vector-mvp-syntax-aux`*

How an HNSW index is persisted, reloaded, and used to serve `ORDER BY DISTANCE(v, ?) LIMIT k`.
Written against the upstream HNSW class (`vector-common/hnsw.h`, PS-11266 + PS-11267), which
owns the graph algorithm and deliberately leaves storage, identity and transactions to its
user. This document is that user's half.

Everything below follows from **two decisions**:

1. **The aux table stores the base row's primary key** (`base_pk`), rather than the base table
   carrying an index on the hidden label column.
2. **Aux writes ride a sub-transaction that commits on its own**, and are *not* rolled back
   when the user's statement rolls back.

## Upstream dependencies

Three things the class does not do yet. All are in progress, and this design assumes them:

| | why we need it |
|---|---|
| **search returns the node id** alongside `base_pk` | without it check ① is impossible, and one vector `UPDATE` silently mis-ranks results (§3.8) |
| **`load_node_cb` can report a missing node** | lets a purge actually delete rows — edges to reclaimed nodes get pruned instead of yielding an uninitialised node (§5) |
| **thread safety** | we serialise writers until it lands. §3.10 states the property its design must give us, which is *not* automatic |

---

## 1. What the base provides

| | |
|---|---|
| `vector-common/hnsw.h` | `insert`, `k_nn_search`, streaming search, lazy per-node loading, the `Persistor` callback concept |
| PS-11264 (DD) | index options — `TYPE` and `WITH (…)` — stored in and restored from the data dictionary |
| PS-11203 (syntax) | `VECTOR KEY (v) TYPE hnsw WITH (M = 16, ef_construction = 200)`, validation, error codes |

The class keeps **no state on disk and no opinion about storage**. It persists no metadata
either: *"users must store dimensions, M and distance kind alongside the graph … mismatches
corrupt the graph or yield wrong search results."* For us that is the DD, via `WITH()`.

---

## 2. What the user sees, and what lands on disk

The SQL surface is the base's. Two things appear that the user did not declare:

**A hidden column on the base table.** `vec_idx_id BIGINT UNSIGNED`, invisible to `SELECT *`
and `SHOW COLUMNS`, holding this row's **label** — the graph's name for it. The `FTS_DOC_ID`
device, with one deliberate difference: **there is no index on it.**

**One auxiliary table per vector index**, `vec_hnsw_<table_id>_<index_id>`, hidden from
`SHOW TABLES` and `INFORMATION_SCHEMA.TABLES`, visible in `INNODB_TABLES`. The `vec_` prefix
is reserved.

---

## 3. Specification

### 3.1 Terminology

| Term | Meaning |
|---|---|
| **label** | A node's identity, and the `id` the class takes. From a per-table counter, stored in the base row's hidden `vec_idx_id`. **Never reused, never zero** — 0 is the class's empty-slot sentinel. |
| **base_pk** | The base row's PRIMARY KEY, held on the node and in its aux row. Single-column `BIGINT UNSIGNED`. Several nodes may share one. |
| **fresh label** | Changing a vector mints a **new** label rather than editing the node, so a `(label, vector)` pair is immutable. |
| **orphan** | A node no visible base row claims — from a rolled-back statement, a delete, or a superseded vector. Filtered at read time; reclaimed only by §5. |

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

No version column, no sequence, no tombstone, no `_dead` companion.

`PRIMARY KEY(id)` is load-bearing, not incidental: `load_node_cb` is a point lookup by node
id, so the label must lead the key. An append-only `PK(seq)` log would give sequential writes
but turn every node fault into a scan — the wrong trade once loading is lazy.

**Record 0 holds the entry point.** The entry point is the node the graph is reached through —
the first node inserted on the topmost layer, the analogue of a B-tree root. `id = 0` can
never collide with a node (the class asserts `id != 0`; our counter starts at 1), so:

```
id = 0    base_pk = <entry point node id>   ← the payload
          vec = ''   level = 0   neighbors = ''
```

Empty blobs are not NULL, so this fits the schema unchanged, and finding it is a PK lookup on
the leftmost record of the clustered index.

It is **not** written once: `update_entry_point_cb` fires whenever a node lands on a new
topmost layer — at most 255 times per index (the layer cap), far fewer in practice since layer
assignment is geometric. So the write is an **upsert**.

Four consequences:

- **Absent means empty, not broken.** No record 0 ⇒ no nodes yet ⇒ do *not* call
  `init_from_entry_point`; build an empty `HNSW` and let the first `insert()` establish it.
- **It names a node**, so §3.5's ordering rule covers it. A record 0 pointing at a node with no
  row leaves `init_from_entry_point` failing its own `assert(m_entry_point->loaded())`.
- **It must never be reclaimed** (§5). The entry-point node will usually *become* an orphan —
  its row deleted, or its vector superseded — so a naive reclaim-orphans sweep would delete the
  one node the graph is entered through.
- **A dangling entry point needs a recovery path**: scan the aux for the highest-`level` node
  and adopt it. That defeats lazy loading exactly once, and beats an unopenable index.

`base_pk` means something different on record 0 than on every other row. The alternative — a
dedicated column — costs 8 bytes on every node row to serve one record. The overload stays,
documented; the only code that must skip record 0 is the recovery scan, since nothing else
ever scans the aux.

### 3.3 The label counter

The class requires a non-zero `id` that does not already exist. We allocate from a per-table
counter and stamp it into the base row's hidden column at INSERT.

**A label must never be reissued**, including after a crash following a rolled-back statement —
a reissued label would alias two rows in one graph. The counter is persisted as a watermark
ahead of what has been handed out, and restored at startup.

### 3.4 The persistor

`Context` is ours to define. It carries the sub-transaction and an error slot, because
callbacks return `void`:

```c
struct Vec_persistor::Context {
  trx_t        *trx;    // the sub-transaction — never the user's
  dict_table_t *aux;    // opened once, MDL held
  THD          *thd;
  dberr_t       err;    // first failure wins; later callbacks short-circuit
};
```

| callback | aux operation |
|---|---|
| `insert_cb(ctx, id, base_pk, q, layer, nbrs)` | INSERT one row |
| `update_neighbors_cb(ctx, id, nbrs)` | UPDATE `neighbors` by PK — the row always exists, `insert_cb` ran first |
| `update_entry_point_cb(ctx, id)` | upsert record 0: UPDATE, and INSERT on `DB_RECORD_NOT_FOUND` |
| `load_node_cb(ctx, hnsw, handle)` | one PK lookup, then `load_set_layer` / `load_set_vec` / `load_set_base_pk` / `load_node_neighbors` |

All of it goes through InnoDB's C API — `ins_node_create` + `row_ins_step` for inserts,
`row_create_update_node_for_mysql` + a hand-positioned `btr_pcur` + `row_upd_step` for
updates. **Never the internal SQL parser**: `pars_sql`/`que_eval_sql` serialise on the global
`pars_mutex`, and this path runs once per rewired neighbour per insert.
`pars_complete_graph_for_exec` is used despite its name — it builds the query-graph fork only
and parses nothing.

**Errors.** The first failure sets `ctx->err`; subsequent callbacks return immediately. The
caller checks after `insert()` returns, rolls the sub-transaction back, and fails the
statement.

### 3.5 The sub-transaction rule

One sub-transaction per `insert()` call, all callbacks writing into it, committed after
`insert()` returns:

```c
Context ctx{ trx_allocate_for_background(), aux, thd, DB_SUCCESS };
trx_start_if_not_started(ctx.trx, true, UT_LOCATION_HERE);

hnsw.insert(label, base_pk, vec_bytes, &ctx);      // fires the callbacks

if (ctx.err != DB_SUCCESS) { trx_rollback_for_mysql(ctx.trx); /* fail the statement */ }
else                         trx_commit_for_mysql(ctx.trx);   // BEFORE the user commits
trx_free_for_background(ctx.trx);
```

Why not ride the user's transaction: **the class has no delete**, so on rollback we cannot
remove the node from memory. If the aux rolled back while memory kept the node, disk and
memory would disagree and a restart would silently change the graph. Not rolling back keeps
them telling the same story.

Everything else follows from one invariant:

> **The aux is a superset of committed base rows.**

Extra nodes are harmless — the read path filters them. A *missing* node is not: the row
exists, kNN never returns it, and nothing reports the gap. So the failure direction must
always be "extra", which pins two rules:

- **the sub-transaction commits before the user's** — a crash in that window leaves an orphan,
  never a gap;
- **a node's own row commits no earlier than anything that names it** — neighbour blobs and
  record 0 alike. Putting every callback of one `insert()` into a single transaction satisfies
  this by construction, since they commit atomically. It is what makes a dangling node id
  impossible.

What this buys beyond rollback coherence: the M neighbour row locks are held for the aux write
rather than for the user's transaction, so insert-insert deadlocks on shared neighbours and
serialisation behind hot hub nodes largely disappear — and the user's undo does not carry M
neighbour updates. There is no deadlock risk against the user's transaction, because the
user's transaction never touches aux rows.

### 3.6 Write paths

| statement | graph | aux |
|---|---|---|
| **INSERT** | `insert(label, pk, vec, &ctx)` | 1 INSERT + 1 UPDATE per rewired neighbour (+ record 0, rarely) |
| **UPDATE** (vector changed) | `insert(fresh_label, pk, new_vec, &ctx)` — the old node stays | as INSERT, for the new label |
| **UPDATE** (PK only) | nothing | `UPDATE aux SET base_pk = ? WHERE id = <current label>` |
| **DELETE** | nothing | nothing |

A NULL vector is not indexed: no node, no aux row. It still consumes a label.

**DELETE writes nothing**, and that is not a shortcut. Check ② requires a reader whose snapshot
predates the delete to still reach the row, so the node *must* stay. The class's missing delete
support and MVCC's requirement coincide.

**PK-only UPDATE** touches only the row's current node. Stale nodes may keep the old
`base_pk`; check ① rejects them anyway.

### 3.7 Read path

```
hits = k_nn_search(q, k, ef_search, &ctx)     → (node_id, base_pk), closest first

per candidate:
    fetch the base row by base_pk, under this reader's own view
    not visible?                → skip
    row.vec_idx_id != node_id?  → skip                    ← check ①
    else emit at the candidate's graph distance

short of k? widen and resume, excluding what was already returned
```

**One dive per candidate.** `base_pk` comes straight from the graph, so the only disk access is
the clustered fetch any index pays to return a row.

### 3.8 How MVCC works — nothing special

There is no vector-specific visibility machinery. The graph is a **versionless candidate
generator**; the base row decides. Four properties, each present for its own reason, add up to
a multi-version store:

| property | why it exists | what it also gives |
|---|---|---|
| fresh label per vector change | counter crash-safety | every historical vector is a distinct, immutable node — *a version* |
| nodes are never removed | the class has no delete | version history is retained |
| `vec_idx_id` is an ordinary column | it is the `FTS_DOC_ID` analogue | it is **versioned by undo**, so the row version a reader sees names the label representing it |
| edges are navigation, not data | HNSW rewires freely | the path to a candidate never affects what may be returned |

One shared graph therefore serves every snapshot: it is a superset of every reader's node set,
and each reader filters it with its own read view. Two checks, and only the first is ours.

**① `row.vec_idx_id == node_id`.** One integer compare on a row the read path already fetched.
One `UPDATE` shows why it is not optional:

```
row 7:  v=[1,0] → label 10
UPDATE t SET v='[0,1]' WHERE id=7   → label 20; node 10 remains

graph:  node 10 → [1,0], base_pk 7      (stale)
        node 20 → [0,1], base_pk 7      (current)
base:   row 7 now carries vec_idx_id = 20

SELECT … ORDER BY DISTANCE(v,'[1,0]') LIMIT 1
   node 10 matches exactly → distance 0 → base_pk 7
   but row 7's vector is now [0,1], far from the query
```

Without ①, row 7 is returned **at the wrong distance** and sorts ahead of rows genuinely near
the query. Not a duplicate and not a missing row — a wrongly ranked one, invisible to the
caller. With ①, `20 != 10` skips the stale node, and node 20 later returns row 7 at its true
distance.

① also covers PK reuse: `DELETE` row 7, then `INSERT` a new row taking PK 7. The stale node
still points at 7; existence proves nothing about identity, but `vec_idx_id` does.

**② include a row deleted after the reader's snapshot.** Free. The node is still in the graph,
`base_pk` is untouched by DELETE, and the fetch under an old view returns the pre-delete
version from undo — whose `vec_idx_id` still reads 10, so ① passes too.

| situation | base fetch | ① | outcome |
|---|---|---|---|
| another transaction's uncommitted insert | not visible | — | skipped |
| deleted, deletion visible to me | not visible | — | skipped |
| deleted **after** my snapshot | visible via undo | passes | **returned — ②** |
| vector updated, stale node | visible | `20 != 10` | skipped |
| PK reused by a different row | visible | `50 != 10` | skipped |
| statement rolled back (orphan) | not found | — | skipped |

**Isolation** is whatever the transaction already has: REPEATABLE READ asks one view for the
transaction, READ COMMITTED a fresh one per statement. Neither needs anything from us — which
is the real argument for this design over versioning the graph.

**Not reachable, by design:** SERIALIZABLE on the index path. Phantom prevention needs
predicate locks over "the k nearest neighbours of q", and ℝᵈ has no key order for gap locks to
hang from. Sessions needing it fall back to the exact path.

### 3.9 Load path

The graph is never loaded wholesale. Cold start reads dims/M/ef from the DD, constructs the
`HNSW`, reads record 0, and calls `init_from_entry_point()`. From there nodes fault in as
searches and inserts touch them:

```c
void load_node_cb(Context *ctx, HNSW &h, LoadNodeHandle handle) {
  uint64_t id = h.load_node_id(handle);
  //  SELECT base_pk, vec, level, neighbors FROM aux WHERE id = ?     (one seek)
  h.load_set_layer(handle, level);
  h.load_set_vec(handle, vec);
  h.load_set_base_pk(handle, base_pk);
  h.load_node_neighbors(handle, ids);     // 0 = empty; stubs for unloaded neighbours
}
```

**The point of this is startup latency, not memory.** There is no full aux scan before the
first query after a restart; warm-up is spread across the queries that need each node. Memory
barely moves: the stub constructor allocates `ALIGN_SIZE(sizeof(Node)) + ALIGN_SIZE(vec_size)`
whether or not the node is ever loaded, so only the `(layer+2)*M` neighbour array is deferred —
under 10% of a node at high dimensions. Loading one node also creates a stub per neighbour id,
so the frontier around a hot region is itself substantial.

Nothing shrinks either: `m_loaded` only goes false → true, there is no unload path, and the
arena has no per-block free. Bounding graph memory is upstream's memory-limits work and needs
both.

**Dict-cache eviction therefore needs no special handling**: it destroys the runtime, the
`HNSW` and its arena together, reclaiming everything at once, and recovery costs one
entry-point load plus faulting rather than a full scan. That is why nothing pins the table.
Teardown order matters — the class *"does not destroy Nodes and must not outlive the
allocator"* — so the graph is destroyed before the arena owning its nodes.

### 3.10 Concurrency

Until upstream thread safety lands, writers are serialised by a per-index latch held across
`insert()`. That is a throughput ceiling, and it is also why **no version column is needed**:
concurrent rewires of the same node cannot happen, so the stale-snapshot race that would demand
one is structurally absent.

When it does land, one property decides whether that stays true:

> A neighbour snapshot handed to `update_neighbors_cb` must be **atomic with the mutation it
> describes** — captured under whatever lock serialises changes to that node's edge list.

If callbacks fire outside that lock, two transactions rewiring the same node can capture
snapshots in one order and reach the aux row in the other, so an older edge list overwrites a
newer one. Memory stays correct; the loss appears only at the next reload, as silent recall
erosion. Recovering from that needs a mutation-order stamp — a per-node counter incremented
under the same lock, compared under the aux row's X lock, stored in a `ver` column.

**We would rather not need that**, so the requirement belongs in the concurrency design rather
than as a repair afterwards. The class already captures *once per touched node at end of
insert* rather than at each mutation site, which is the better shape — it is simply not yet
ordered against other writers.

---

## 4. Limitations and impacts

**Orphans accumulate; the index grows with mutations, not rows.** A rolled-back statement, a
deleted row and a superseded vector all leave a node that MVP never removes. A traversed orphan
costs a full node block in memory, reclaimed only by dropping the index or restarting. §5 is
the answer and is explicitly out of MVP scope.

**Aux state is not transactional with base data.** By design. The aux is durable state
converging on the base table, and the base table is the sole authority on what is real.
`SELECT COUNT(*)` on the aux will not match the base table, and that is correct behaviour.

**Writers are serialised** (§3.10) until upstream threading lands.

**A failed aux write fails the statement** — committing a base row whose node never landed
would violate the superset invariant in the unsafe direction.

**INSTANT ADD/DROP COLUMN, IMPORT and DISCARD** are refused on vector-indexed tables;
`ADD VECTOR INDEX` is COPY-only; a vector index's TYPE cannot be changed in place.

---

## 5. Post-MVP: reclaiming orphans

Not in MVP, but the shape is decided, because "never delete" is not shippable long term.

A label is reclaimable once **no active read view can see any row version carrying it** — the
exact negation of the condition ② needs, so a correct purge preserves ② with no separate rule.

The three retirement events are ours to observe, and only we see all of them:

| event | who knows |
|---|---|
| DELETE | us, at delete time |
| vector UPDATE — old label retired | us, when we mint the fresh label |
| **INSERT rolled back** | **us, at rollback — nothing else ever knows** |

That last row rules out piggybacking on InnoDB purge: purge is driven by the *update* undo
history, and insert undo is discarded at commit and at rollback (*"knowledge of inserts is not
needed after a commit or rollback"*), so a rolled-back INSERT produces no purge event at all —
and that is precisely the orphan class this design manufactures deliberately. Purge also
materialises only a partial row of *indexed* fields, and `vec_idx_id` is in no index.

So: a `_dead(label, retired_trx_id)` work-list written at all three points, reclaimed when
`retired_trx_id` predates the oldest active view. It is a **hint, never an authority** —
visibility stays with the base row and ① — so being stale in either direction costs a wasted
probe or a delayed reclaim, never a wrong row.

Two upstream dependencies: `load_node_cb` must be able to report a missing node (else deleting
rows re-creates dangling references), and delete support would stop purged nodes from being
re-referenced by later rewires. And record 0's node must be pinned against reclamation.

---

## 6. What this deliberately does not do

- **No on-disk traversal.** The graph stays resident; disk reconstructs it, one node at a time.
- **No version or sequence column** — see §3.10 for the condition that keeps this true.
- **No `_dead` table, no tombstones** in MVP. DELETE writes nothing.
- **No dict-cache pinning.** Lazy loading makes eviction cheap enough to ignore.
- **One vector index per table**, and a single-column integer PRIMARY KEY.
