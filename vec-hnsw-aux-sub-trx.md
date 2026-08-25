# Persisting the HNSW Vector Index

*Percona Server · InnoDB · `VECTOR KEY … TYPE hnsw`*

---

## 1. What already exists

This design builds on work that is already done or in flight. None of it is restated here
beyond what a reader needs.

| Component | What it provides |
|---|---|
| **Vector data type** | `VECTOR(n)` columns, `STRING_TO_VECTOR()`, `DISTANCE()` |
| **Distance functions** (`vector-common/vector_distance.*`) | L2 / inner-product kernels, with AVX-512 paths |
| **Index syntax** (PS-11203) | `VECTOR KEY (v) TYPE hnsw WITH (M = 16, ef_construction = 200)`, its validation and error codes |
| **Data dictionary** (PS-11264) | the index `TYPE` and its `WITH (…)` parameters are stored in, and restored from, the DD |
| **The HNSW class** (PS-11266) | `vector-common/hnsw.h` — the graph algorithm: insert, k-NN search, streaming search |
| **Persistence APIs** (PS-11267) | the `Persistor` callback concept, lazy per-node loading, and cold start from a persisted entry point |

The HNSW class owns the algorithm and nothing else. It has no on-disk representation, no
opinion about storage, and it does not even persist its own parameters:

> *Index metadata is not persisted by HNSW itself. The class users must store it alongside the
> graph (at minimum: vector dimensions, M, distance kind).*

Three additions to that class are in progress and this design depends on them: search
returning the **node id** alongside `base_pk` (§9), `load_node_cb` being able to report a
**missing node** (§16), and **thread safety** (§17).

---

## 2. Goal

The graph lives in memory. This design gives it a home on disk, so that:

- a vector index survives server restart without re-inserting every row;
- the graph is crash-safe to the same standard as any InnoDB index;
- queries served from the graph obey MySQL's transaction and isolation rules;
- every DDL operation leaves the index consistent.

Concretely: **store what the HNSW class holds in memory into an InnoDB table, and rebuild the
graph from it on demand.**

---

## 3. Terminology

| Term | Meaning |
|---|---|
| **node** | One vector in the graph. The unit HNSW inserts, links and searches. |
| **label** | A node's identity — the `id` the HNSW class takes. A `BIGINT UNSIGNED` from a per-table counter. Never reused, never zero (0 is the class's empty-slot sentinel). |
| **base_pk** | The PRIMARY KEY of the base-table row a node describes. |
| **layer / level** | How many HNSW layers a node participates in. Assigned randomly at insert; geometrically distributed, so high layers are rare. |
| **neighbours** | A node's outgoing edges, one list per layer. The graph *is* these lists. |
| **entry point** | The node searches start from — the first node inserted on the topmost layer. The analogue of a B-tree root. |
| **aux table** | The InnoDB table holding one row per node. |
| **fresh label** | Changing a row's vector mints a *new* label rather than editing the existing node. |
| **orphan** | A node that no visible base row claims. |

---

## 4. Where the objects live

Three layers, each owning one thing.

```
   dict_table_t  (the user's table)
        │
        ├── dict_index_t  ── the vector index: its id, its TYPE
        │
        └── Vec_runtime *vec ─────────┐   per-table, created on first use
                                      │
                            ┌─────────┴─────────┐
                            │  the HNSW graph   │   in memory
                            │  + arena          │
                            │  + Persistor      │
                            └───────────────────┘
                                      │  callbacks
                                      ▼
                            vec_hnsw_<tid>_<iid>    on disk
```

**`dict_index_t`** is the index itself, as the dictionary knows it: its `id`, and the fact that
it is a vector index. It carries no graph.

**`dict_table_t::vec`** is a pointer to the per-table runtime — the analogue of
`dict_table_t::fts`. It is `nullptr` until something first opens the index, and it owns the
`HNSW` object, the arena its nodes are allocated from, the persistor, and the index parameters
read back from the DD.

**The aux table** is named `vec_hnsw_<table_id>_<index_id>`, hidden from `SHOW TABLES` and
`INFORMATION_SCHEMA.TABLES`, visible in `INNODB_TABLES`. The `vec_` prefix is reserved, so a
user cannot create a table that collides with a computed aux name.

**A hidden column on the base table** carries the label: `vec_idx_id BIGINT UNSIGNED`,
invisible to `SELECT *` and `SHOW COLUMNS`. This is the same device FTS uses with
`FTS_DOC_ID`. It is what lets a row and its node find each other.

---

## 5. The `Vector_index` seam

HNSW will not be the only vector index type. The engine is therefore structured so that adding
a second type is an addition rather than an edit — no call site outside the implementation
changes when one arrives.

```c
class Vector_index {                       // stateless singleton, one per TYPE
  virtual Vec_index_type type() const = 0;
  virtual void    open(dict_table_t *, field_no, dims, M, ef) const = 0;
  virtual dberr_t insert(trx, table, thd, label, base_pk, vec) const = 0;
  virtual dberr_t knn(table, thd, query, dims, k, ef, hits) const = 0;
  virtual dberr_t load(dict_table_t *, THD *) const = 0;
  virtual void    close(dict_table_t *) const = 0;
};

const Vec_type_entry vec_type_registry[] = {
    {"hnsw", &vec_hnsw_singleton},         // adding a TYPE is one row here
};
```

Two rules keep the seam honest:

- **Implementations are stateless.** All per-index state lives in the runtime hanging off
  `dict_table_t`, so there is no object lifetime to manage and no per-table singleton.
- **The runtime is typed as a generic base.** `dict_table_t::vec` is a `Vec_runtime *`;
  hnsw's own struct derives from it, and only the implementation that allocated a runtime may
  interpret the subtype. The downcast is file-local to the hnsw implementation, so the
  compiler enforces the boundary.

Dispatch has exactly two sources, and neither guesses:

| situation | resolved by |
|---|---|
| a runtime is open | `table->vec->impl` — an open runtime records the implementation that created it |
| no runtime yet (open, build) | the `TYPE` token from the KEY definition, resolved through the registry |

An unresolvable token is an error, never a default. A resolver that guesses would hand one
implementation's table to another as soon as a second type exists.

---

## 6. The aux table

**One row per node, updated in place.**

```sql
CREATE TABLE vec_hnsw_<tid>_<iid> (
  id        BIGINT UNSIGNED PRIMARY KEY,  -- the label
  base_pk   BIGINT UNSIGNED NOT NULL,     -- the base row this node describes
  vec       BLOB NOT NULL,                -- dims * 4 bytes
  level     TINYINT NOT NULL,             -- top layer for this node
  neighbors BLOB NOT NULL                 -- neighbour ids, 0 = empty slot
)
```

`PRIMARY KEY(id)` is load-bearing. Nodes are fetched one at a time, by label (§15), so the
label must lead the key.

**Record 0 holds the entry point.** Since 0 is never a real label, that row is free to use as
index metadata:

```
id = 0    base_pk = <entry point node id>      ← the payload
          vec = ''    level = 0    neighbors = ''
```

Empty blobs are not NULL, so this needs no schema change, and reading it is a lookup of the
leftmost record of the clustered index.

Two things follow. **No record 0 means the index is empty**, not broken — a freshly created
index has no nodes and no entry point until the first insert. And **record 0 must be written
after the node it names**, since it is a reference like any neighbour list (§19).

---

## 7. The persistence callbacks

The HNSW class calls out to a `Persistor` we supply. `Context` is ours to define and is passed
through every call; the class treats it as opaque.

```c
struct Vec_persistor::Context {
  trx_t        *trx;    // the sub-transaction the aux writes ride
  dict_table_t *aux;    // opened once, MDL held
  THD          *thd;
  dberr_t       err;    // callbacks return void — the first failure is recorded here
};
```

| Callback | When it fires | What we do |
|---|---|---|
| `insert_cb(ctx, id, base_pk, q, layer, nbrs)` | once, for the newly inserted node | INSERT its aux row |
| `update_neighbors_cb(ctx, id, nbrs)` | once per node whose edges the insert changed | UPDATE that row's `neighbors` |
| `update_entry_point_cb(ctx, id)` | when a node lands on a new topmost layer | upsert record 0 |
| `load_node_cb(ctx, hnsw, handle)` | when a search or insert touches a node that is not in memory | read its aux row and fill the node |

`update_entry_point_cb` is rare: at most 255 times over an index's lifetime — the layer cap —
and far fewer in practice, since layer assignment is geometric.

---

## 8. How we write to the aux table

Two mechanisms, both chosen for specific reasons.

**The query-graph C API, not the SQL parser.** Aux DML is built directly:
`ins_node_create` + `row_ins_step` for inserts, `row_create_update_node_for_mysql` with a
hand-positioned cursor + `row_upd_step` for updates. This gives full redo, undo, and row
locking — including the standard lock-wait and deadlock handling via
`row_mysql_handle_errors`, so an aux write behaves like any other DML under contention.
InnoDB's internal SQL parser (`pars_sql` / `que_eval_sql`) is **not** used: it serialises every
statement on the global `pars_mutex`, and this path runs once per rewired neighbour on every
insert.

**A sub-transaction, not the user's.** Aux writes run on their own transaction, which commits
independently of the statement that triggered it. §13 explains why, and §19 states the rules
that make it safe.

---

## 9. INSERT

A user runs:

```sql
INSERT INTO t (id, v) VALUES (7, STRING_TO_VECTOR('[1,0,0,0]'));
```

**1. The server assigns a label.** Before the base row is written, the counter hands out the
next label — say 10 — and it is stamped into the row's hidden `vec_idx_id` column. The row is
inserted by the ordinary InnoDB path, on the user's transaction.

**2. We start a sub-transaction and call the graph.**

```c
Context ctx{ trx_allocate_for_background(), aux, thd, DB_SUCCESS };
hnsw.insert(/*id=*/10, /*base_pk=*/7, vector_bytes, &ctx);
```

**3. The class does its work, then calls us back.** It picks a layer for the new node, searches
for its nearest neighbours, links them mutually, and prunes each affected neighbour's edge list
back to `M`. Only when all of that is done does it fire callbacks:

```
insert_cb(ctx, 10, 7, [1,0,0,0], layer, nbrs)   →  INSERT INTO aux VALUES (10, 7, …)
update_neighbors_cb(ctx, 5,  nbrs_of_5)         →  UPDATE aux SET neighbors=… WHERE id=5
update_neighbors_cb(ctx, 12, nbrs_of_12)        →  UPDATE aux SET neighbors=… WHERE id=12
      … one per neighbour whose edges changed …
update_entry_point_cb(ctx, 10)                  →  upsert record 0   (only if layer is new)
```

So one INSERT produces **one aux insert plus roughly M aux updates**.

**4. We commit the sub-transaction**, before the user's statement completes.

```c
if (ctx.err != DB_SUCCESS) { trx_rollback_for_mysql(ctx.trx); /* fail the statement */ }
else                         trx_commit_for_mysql(ctx.trx);
```

If any aux write failed, the first failure is in `ctx.err`, later callbacks short-circuit, the
sub-transaction rolls back and the statement fails — taking the base row with it.

A `NULL` vector is not indexed at all: no node, no aux row. It still consumes a label.

---

## 10. UPDATE

**Changing the vector** does not edit the node. Nodes are immutable: HNSW cannot safely move a
point once its neighbours are linked to it. So a new label is minted and inserted, and the old
node is left alone.

```sql
UPDATE t SET v = STRING_TO_VECTOR('[0,1,0,0]') WHERE id = 7;
```

```
base row 7:  vec_idx_id  10 → 20
graph:       node 10 stays (vector [1,0,0,0])
             node 20 added (vector [0,1,0,0])
aux:         row 10 untouched; row 20 inserted; ~M neighbour rows updated
```

Both nodes now carry `base_pk = 7`. §14 explains how a query tells them apart.

**Changing only the primary key** does not touch the graph at all — the vector has not moved.
It does need the row's *current* node to be re-pointed:

```sql
UPDATE aux SET base_pk = <new pk> WHERE id = <current label>
```

Older nodes for the same row keep the stale `base_pk`; §14 shows why that is harmless.

---

## 11. DELETE

```sql
DELETE FROM t WHERE id = 7;
```

**Nothing happens to the graph, and nothing is written to the aux table.**

That is not an omission. A transaction that started before this delete is still entitled to see
row 7, and the only way it can reach the row through the index is via node 10 — so the node
must stay. Removing it would break isolation, not tidy up.

Deleted nodes are filtered at read time (§14), so they can never be *returned*; they simply
remain as part of the graph's structure, still usable as routers during traversal.

The cost is that dead nodes accumulate; §20 covers what that means and how it is eventually
reclaimed.

---

## 12. SELECT

```sql
SELECT id FROM t ORDER BY DISTANCE(v, '[1,0,0,0]', 'EUCLIDEAN') LIMIT 5;
```

```
1. k_nn_search(query, k, ef_search, &ctx)  →  (node_id, base_pk) pairs, closest first
2. for each candidate:
       fetch the base row by base_pk, under this reader's own read view
       row not visible?              → skip
       row.vec_idx_id != node_id?    → skip
       otherwise return it, at the candidate's graph distance
3. short of k? widen the search and resume, excluding what was already returned
```

The graph search is pure memory (plus any node faults, §15). The only disk access per candidate
is the base-row fetch — the same lookup any secondary index performs to return a row.

The two skip conditions are what make the index correct under concurrency, and §14 explains
them.

---

## 13. Rollback, and why orphans are acceptable

If the statement in §9 rolls back, the base row disappears — but **the aux rows stay**, because
they were committed on their own transaction. Node 10 remains in the graph and in the aux, now
describing a row that does not exist. That is an *orphan*.

This is deliberate. The HNSW class has no delete operation, so on rollback we **cannot** remove
the node from memory. If the aux rolled back while memory kept the node, disk and memory would
disagree, and the graph would silently change shape at the next restart. Leaving both in place
keeps them consistent.

Orphans are harmless because the read path already filters them: a candidate whose `base_pk`
resolves to no visible row is skipped (§12). The same filter handles deleted rows and
superseded vectors, so orphans need no special case.

The direction of the failure is what matters. An **extra** node costs a wasted candidate slot.
A **missing** node would mean a committed row that the index never returns — a wrong answer
with nothing to report it. §19 states the rules that keep the error always on the harmless side.

---

## 14. How MVCC works

There is no vector-specific visibility machinery. The graph is a **candidate generator** and
the base table is the authority: the graph proposes, the row decides.

This works because of four properties that each exist for their own reason:

| Property | Why it exists | What it also gives |
|---|---|---|
| a vector change mints a fresh label | nodes are immutable | every historical vector is a distinct node — in effect, a version |
| nodes are never removed | HNSW cannot delete safely | that version history is retained |
| `vec_idx_id` is an ordinary column | it is the `FTS_DOC_ID` device | it is **versioned by undo**, so the row version a reader sees names the node that represents it |
| edges are navigation, not data | HNSW rewires freely | the path taken to a candidate never affects what may be returned |

So one shared graph serves every transaction. It is a *superset* of what any reader should see,
and each reader narrows it with its own read view. Two checks do that.

**Check ① — `row.vec_idx_id == node_id`.** Consider the update from §10, and a query for the
*old* vector:

```
graph:   node 10 → [1,0,0,0], base_pk 7      (stale — that vector is gone)
         node 20 → [0,1,0,0], base_pk 7      (current)
base:    row 7 now carries vec_idx_id = 20

SELECT … ORDER BY DISTANCE(v,'[1,0,0,0]') LIMIT 1
    node 10 is an exact match → distance 0 → base_pk 7
    but row 7's vector today is [0,1,0,0], which is far from the query
```

Returning row 7 here would rank it at a distance belonging to a vector it no longer has —
placing it ahead of rows that genuinely are near the query. Not a duplicate, not a missing row:
a wrongly ordered one, with nothing to signal it. Check ① rejects it, because `20 != 10`, and
node 20 later returns row 7 at its true distance.

The same check handles a recycled primary key: if row 7 is deleted and a different row is
inserted with `id = 7`, the stale node still points at PK 7 — but the new row's `vec_idx_id` is
not 10.

**Check ② — a row deleted after the reader's snapshot.** This one needs no code. The node is
still in the graph, its `base_pk` was never touched by the delete, and the fetch under an older
read view returns the pre-delete version of the row from undo — whose `vec_idx_id` still reads
10, so check ① passes as well.

Every case a concurrent workload produces:

| Situation | base-row fetch | check ① | Result |
|---|---|---|---|
| another transaction's uncommitted insert | not visible | — | skipped |
| deleted, and the delete is visible to me | not visible | — | skipped |
| deleted **after** my snapshot | visible, from undo | passes | **returned** |
| vector updated; this is the stale node | visible | `20 != 10` | skipped |
| primary key reused by a different row | visible | fails | skipped |
| the inserting statement rolled back | not found | — | skipped |

**Isolation levels need nothing from us.** REPEATABLE READ asks one read view for the whole
transaction; READ COMMITTED asks a fresh one per statement. Both simply filter the same shared
graph differently.

SERIALIZABLE is not reachable on the index path: phantom prevention would need predicate locks
over "the k nearest neighbours of q", and there is no key order in ℝᵈ for gap locks to attach
to. Such sessions fall back to the exact path.

---

## 15. Lazy loading

**The graph is never loaded in one go.** The intention is startup latency: without this, the
first query on an index after a restart would pay for reading every node before returning a
single row.

Cold start does the minimum:

```
1. read dims, M, ef_construction from the DD
2. construct an empty HNSW with exactly those parameters
3. read record 0 → the entry point label
       absent?  → the index is empty; stop here
4. init_from_entry_point(entry_label, &ctx)   → loads exactly ONE node
```

The entry point is the graph's root, so that single node is enough to start traversing.
Everything else arrives on demand:

```c
void load_node_cb(Context *ctx, HNSW &h, LoadNodeHandle handle) {
  uint64_t id = h.load_node_id(handle);            // which node is wanted
  //  SELECT base_pk, vec, level, neighbors FROM aux WHERE id = ?      one PK lookup
  h.load_set_layer(handle, level);
  h.load_set_vec(handle, vec);
  h.load_set_base_pk(handle, base_pk);
  h.load_node_neighbors(handle, ids);              // creates stubs for unloaded neighbours
}
```

Loading a node creates **stubs** for each of its neighbours — nodes that exist by id but hold
no data. A stub is filled the first time a traversal actually reaches it. A search therefore
descends one path from the entry point to layer 0, faulting roughly one node per layer plus the
neighbourhood it examines at the bottom — for ten million rows at `M = 16`, on the order of six
nodes, not ten million.

Two properties worth knowing:

- **This bounds latency, not memory.** A stub already allocates its node block including vector
  space; only the neighbour array is deferred. Nor does anything shrink: a node never returns to
  the unloaded state, and the arena has no per-block free.
- **The parameters must match** what the index was built with. The class is explicit that a
  mismatch in dimensions or `M` corrupts the graph or yields wrong results, which is why they
  come from the DD rather than from a default.

---

## 16. Dictionary cache eviction

A `dict_table_t` can be evicted when nothing references it. For a vector-indexed table that
means the runtime, the `HNSW` object and its arena are all destroyed together, and every node's
memory is reclaimed in a single step.

Nothing needs to be saved first — everything in the graph is already in the aux table. The next
statement that touches the table opens the runtime again and starts from the entry point, as in
§15. Because recovery is one node load rather than a full scan, eviction is cheap enough that
vector-indexed tables need no special protection from it.

The one constraint is teardown order. The class states that it *"does not destroy Nodes and must
not outlive the allocator"*, so the graph must be destroyed before the arena its nodes live in.

---

## 17. Concurrency

The HNSW class is not yet thread-safe, so writers are serialised: a per-index latch is held
across `insert()`. Reads are unaffected by this except that they may wait behind a writer.

That serialisation is also why the aux needs no version or sequence column. When two
transactions rewire the same node concurrently, their aux writes can reach the row in the
opposite order from the in-memory mutations, so an older edge list can overwrite a newer one —
losing an edge on disk that memory still has, and surfacing only at the next reload as slightly
degraded recall. Serialised writers make that race impossible.

When thread safety does land, one property decides whether that remains true:

> A neighbour list handed to `update_neighbors_cb` must be captured **atomically with the
> mutation it describes**, under whatever lock serialises changes to that node's edges.

If it is, ordering is preserved and nothing more is needed. If callbacks fire outside that lock,
the aux needs a mutation-order stamp: a per-node counter incremented under the same lock, stored
in the row, and compared before overwriting.

---

## 18. DDL

| Operation | Effect on the index |
|---|---|
| `CREATE TABLE … VECTOR KEY` | adds the hidden `vec_idx_id` column, creates the aux table, registers it in the DD |
| `DROP TABLE` | drops the aux table with the parent |
| `TRUNCATE TABLE` | drop and recreate — the aux is re-created empty, and the label counter restarts |
| `RENAME TABLE` | same schema: nothing to do. Cross-schema: the aux moves with the parent |
| `DROP INDEX` | drops that index's aux table; the hidden column is retained |
| `ALTER TABLE … ADD VECTOR KEY` | see below |
| `IMPORT` / `DISCARD TABLESPACE` | refused on vector-indexed tables |
| `ALTER … ALGORITHM=INSTANT` (ADD/DROP COLUMN) | refused on vector-indexed tables |

Aux tables are hidden from user-facing catalogue views but registered in the DD like any table,
so they participate normally in crash recovery and DDL logging.

**`IMPORT` is refused** because an imported tablespace carries base rows that no aux table
describes. The alternatives were an index that silently omits every imported row, or a rebuild
the user did not ask for inside a metadata-only statement. Refusing keeps the aux and the base
rows from ever disagreeing.

**A vector index's `TYPE` cannot be changed in place.** Changing it means dropping the index and
adding it back, which rebuilds the graph — the stored aux contents belong to the old
implementation.

---

## 19. `ALTER TABLE … ADD VECTOR KEY`

Adding a vector index to a table with existing rows is performed by **table copy**, not
in-place.

```sql
ALTER TABLE t ADD VECTOR KEY vk (v) TYPE hnsw WITH (M = 16);
```

The copy path already rewrites every row into a new table, and each of those rows travels the
ordinary INSERT path of §9 — a label is assigned, `hnsw.insert()` is called, callbacks populate
the aux. So the graph is built as a side effect of the copy, with no separate build phase and no
second code path to keep correct.

In-place is refused deliberately. It would have to build the graph while concurrent writers
mutate the table, and reconcile a partially built index with changes arriving behind it. The
copy is slower and obviously correct.

---

## 20. Durability and crash recovery

**The aux table is an ordinary InnoDB table.** Its writes are redo-logged and undo-logged like
any other, so recovery restores it with no vector-specific machinery. There is no separate log,
no checkpoint of the graph, and nothing to replay by hand.

**The graph itself is never persisted as such** — only the rows it can be rebuilt from. After a
crash there is no graph in memory; the first statement to touch the index rebuilds it from the
entry point (§15).

**The label counter** is persisted as a watermark that runs ahead of the labels actually handed
out, so a crash can never cause a label to be reissued. A reissued label would give two rows the
same identity in one graph.

**A crash between the two commits** — the aux sub-transaction and the user's transaction — is
the interesting window, and the ordering rule in §21 makes it safe: the sub-transaction commits
first, so a crash in between leaves an orphan node, never a committed row without one.

---

## 21. The rules

Three invariants. Everything above is arranged so that they hold.

**1. The aux is a superset of committed base rows.**
Extra nodes are filtered at read time and cost only a wasted candidate. A missing node is a
committed row the index never returns — a wrong answer with nothing to report it. Every choice
in this design puts the error on the harmless side.

**2. The sub-transaction commits before the user's transaction.**
This is what keeps rule 1 true across a crash. One sub-transaction is used per `insert()` call,
covering the node's own row, every neighbour update, and record 0. They therefore commit
atomically, which also satisfies the ordering requirement that **a node's row must exist before
anything references it** — neighbour lists and record 0 alike. A dangling reference is
impossible by construction.

One transaction per `insert()` rather than one per callback: while writers are serialised (§17)
there is no contention to relieve by committing sooner, and a single transaction gives
atomicity for free. This is worth revisiting when the class becomes thread-safe.

**3. Aux writes never touch the parent table.**
The sub-transaction reads and writes the aux table only. The user's transaction never locks an
aux row, and the sub-transaction never locks a base row, so the two can never deadlock against
each other.

---

## 22. Limitations

**Dead nodes accumulate.** A deleted row, a superseded vector and a rolled-back insert all leave
a node behind, and MVP never removes them. The index therefore grows with the number of
*mutations* rather than the number of rows. They are reclaimed today only by dropping the index
or rebuilding it.

Reclaiming them properly is a later phase. The rule is known — a label may be removed once no
active read view can see any row version carrying it, which is exactly the negation of the
condition check ② depends on — and it needs a record of retirement events, since one of them (a
rolled-back insert) leaves no trace anywhere else in the engine.

**Writers are serialised** until the class becomes thread-safe (§17).

**The aux is not transactional with the base table.** By design (§13). A count of aux rows will
not equal a count of base rows, and that is correct behaviour rather than corruption.

**One vector index per table**, and a single-column integer primary key.
