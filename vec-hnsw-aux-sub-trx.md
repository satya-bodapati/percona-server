# Persisting the HNSW Vector Index

*Percona Server · InnoDB · `KEY (v) TYPE hnsw`*

---

## 1. What already exists

This design builds on work that is already done or in flight. None of it is restated here
beyond what a reader needs.

| Component | What it provides |
|---|---|
| **Vector data type** | `VECTOR(n)` columns, `STRING_TO_VECTOR()`, `DISTANCE()` |
| **Distance functions** (`vector-common/vector_distance.*`) | L2 / inner-product kernels, with AVX-512 paths |
| **Index syntax** (PS-11203) | `KEY (v) TYPE hnsw WITH (M = 16, ef_construction = 200)`, its validation and error codes |
| **Data dictionary** (PS-11264) | the index `TYPE` and its `WITH (…)` parameters are stored in, and restored from, the DD |
| **The HNSW class** (PS-11266) | `vector-common/hnsw.h` — the graph algorithm: insert, k-NN search, streaming search |
| **Persistence APIs** (PS-11267) | the `Persistor` callback concept, lazy per-node loading, and cold start from a persisted entry point |

There is no `VECTOR KEY` keyword. A key becomes `KEYTYPE_VECTOR` (`parse_tree_nodes.cc:2194`)
because it named a `TYPE` that is not BTREE, RTREE or HASH.

The HNSW class owns the algorithm and nothing else. It has no on-disk representation, no
opinion about storage, and it does not even persist its own parameters:

> *Index metadata is not persisted by HNSW itself. The class users must store it alongside the
> graph (at minimum: vector dimensions, M, distance kind).*

Three additions to that class are needed by this design and **none of them has landed**:
search returning the **node id** alongside `base_pk` (§24 — the GA blocker), `load_node_cb`
being able to **report failure** (§25), and **thread safety** (§17, §32). Each is a section in
Part II with the code and an example.

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
| **label** | A node's identity — the `id` the HNSW class takes. A `BIGINT UNSIGNED` from a per-index counter. Never reused, never zero (0 is the class's empty-slot sentinel). |
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
        └── dict_index_t  ── the vector index: its id, its TYPE
                 │
                 └── Vec_runtime *vec ──┐   per-index, created on first use
                                        │
                              ┌─────────┴─────────┐
                              │  the HNSW graph   │   in memory
                              │  + arena          │
                              │  + Persistor      │
                              └───────────────────┘
                                        │  callbacks
                                        ▼
                              percona_vec_hnsw_<tid>_<iid>    on disk
```

**`dict_index_t::vec`** is a pointer to the runtime. It is `nullptr` until something first opens
the index, and it owns the `HNSW` object, the arena its nodes are allocated from, the persistor,
and the index parameters read back from the DD.

### Why the runtime hangs off the index, not the table

Everything the runtime holds is a property of one index: dimensions, metric, `M`,
`ef_construction`, the entry point, the label space, the graph itself. Two vector indexes on a
table share none of it, and the aux table is already keyed by index —
`percona_vec_hnsw_<table_id>_<index_id>`. Lifetime agrees: `DROP INDEX` destroys exactly one
runtime, and `dict_mem_index_free()` is where it is released.

FTS looks like a counter-example and is not. `table->fts` holds genuinely table-scoped state —
one shared cache, one `FTS_DOC_ID` column, one doc-id counter — *plus* a list of the indexes that
share it; per-index state lives in its `FTS_INDEX_TABLE` aux tables. Vector has no shared
machinery of that kind. Its one table-scoped object is the hidden `percona_vec_aux_id` column,
and that is what limits a table to a single vector index (§23).

Two consequences follow from the placement:

**The pointer must be raw.** `dict_index_t` is never constructed or destructed — the memory is
zeroed and `dict_mem_fill_index_struct()` stands in for a constructor — so `Vec_runtime *vec` is
zero-initialised by that memset and released by hand in `dict_mem_index_free()`, exactly as
`destroy_fields_array()` already is. No smart pointer, no non-POD member.

**A vector index still carries its key part.** `DICT_VECTOR` is a bit in `dict_index_t::type`,
beside `DICT_FTS` and `DICT_SPATIAL` — the family with no real B-tree — and `dict0dd.cc:2993`
asserts:

```c
ut_ad(!!(type & (DICT_FTS | DICT_VECTOR)) == (n_uniq == 0));
```

That constrains `n_uniq`, not `n_fields`. `dict_index_add_col()` runs for a vector key on both
the CREATE and DD-open paths, so `index->get_field(0)->col` **is** the indexed column. Nothing
may search for it instead: `VECTOR`, `BLOB`, `TEXT` and `JSON` all collapse to `DATA_BLOB`, so no
type test can distinguish them. Its `prefix_len` is 1 and means nothing.


**The aux table** is named `percona_vec_hnsw_<table_id>_<index_id>`, hidden from `SHOW TABLES` and
`INFORMATION_SCHEMA.TABLES`, visible in `INNODB_TABLES`. Nothing about the name is reserved from
users. `vec_aux_parse_table_name()` recognises an aux table by parsing the whole shape — the
`percona_vec_` prefix, a known index-type token, then two object ids — so a user table that
merely begins with those characters is never mistaken for one.

**A hidden column on the base table** carries the label: `percona_vec_aux_id BIGINT UNSIGNED`,
invisible to `SELECT *` and `SHOW COLUMNS`. This is the same device FTS uses with
`FTS_DOC_ID`. It is what lets a row and its node find each other.

---

## 5. The type seam

HNSW will not be the only vector index type, so the engine keeps a seam for a second one. The
seam is deliberately thin: a type token that survives on disk, and a type-erased runtime pointer.

```c
enum class Vec_index_type : uint8_t { HNSW = 0 };   // vec0aux.h

struct Vec_runtime {                                // vec0index.h
  virtual ~Vec_runtime() = default;                 // ...the whole base
};
```

`dict_index_t::vec` is a `Vec_runtime *`; `vec_t` derives from it and the downcast is file-local,
so the compiler — not a convention — stops any other translation unit interpreting the subtype.
There are no virtuals to dispatch on because there is nothing yet to dispatch between.

The token is carried where it must be durable: in the aux table name,
`percona_vec_<type>_<table_id>_<index_id>`, parsed back by `vec_aux_parse_table_name()`. A second
type therefore costs an enumerator and a token, and existing aux tables keep naming themselves
unambiguously.

What a second type would add is behaviour dispatch, which does not exist: `vec_insert_row()`,
`vec_update_row()` and `vec_runtime_open()` call HNSW directly today. Turning those three call
sites into a vtable on `Vec_runtime`, or a registry keyed by `Vec_index_type`, is a contained
change precisely because the runtime already hangs off `dict_index_t` (§4) and the type already
survives on disk.

The parameter side is upstream's, and is a variant rather than a vtable:

```c
struct HnswParam { int M{25}; int max_elements{10000};
                   int ef_construction{200}; std::string_view metric{"euclidean"}; };
using VectorIndexParam = std::variant<std::monostate, HnswParam>;
```

Adding a type there means adding an alternative. `parse_options()` is split into a shared
implementation with two overloads — `Key_spec` for DDL, `KEY` for table open — so the same parse
both validates at DDL time and carries `M` and `ef_construction` into the runtime at open.
`max_elements` is the odd one out: a field nothing sets and nothing reads (§27).

One rule keeps the seam honest: **implementations are stateless.** All per-index state lives in
the runtime, so there is no object lifetime to manage and no per-index singleton. The HNSW class
imposes the same rule on the persistor (§7).

## 6. The aux table

**One row per node, updated in place.**

```sql
CREATE TABLE percona_vec_hnsw_<tid>_<iid> (
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

## 7. Wiring the persistor

**There is no registration call.** The class takes the persistor as a *template parameter* and
holds one as a member:

```c
template <typename ArenaAllocator, typename Persistor>
class HNSW {
  using PersistorContext = typename Persistor::Context;
  ...
  ArenaAllocator m_allocator;
  Persistor      m_persistor;      // default-constructed
};
```

So "registering the callbacks" is simply instantiating the template:

```c
using Vec_hnsw = HNSW<Vec_arena, Vec_persistor>;
```

Three consequences follow from that being compile-time rather than runtime:

- **No function pointers and no virtual dispatch.** The calls inline.
- **The persistor must be stateless.** It is default-constructed and shared by every call, so it
  can hold nothing per-call, per-transaction or per-thread. Everything of that kind travels in
  `Context`, which the class passes through untouched.
- **`Context` is ours to define** — the class only knows it as `typename Persistor::Context`.

### How a call reaches our function

Nothing is looked up at run time. The chain is entirely compile-time:

```
using Vec_hnsw = HNSW<Vec_arena, Vec_persistor>;     ← this line is the registration

  1. the compiler substitutes Vec_persistor for the Persistor parameter
  2. the member `Persistor m_persistor;` becomes a real Vec_persistor object
  3. inside insert(), the class writes
         m_persistor.update_neighbors_cb(ctx, neighbor->id(), neighbor_ids(neighbor));
     which is an ordinary member call on that object, resolved by name
```

If `Vec_persistor` has no method of that name with compatible parameters, **the code does not
compile**. That failure is the only "registration check" there is — no vtable, no function
pointer, no registry, and nothing to forget to wire up at run time.

### What the callbacks must look like

One wrinkle shapes the signatures. The neighbour range the class passes is
`HNSW<A,P>::NeighborIdRange` — a type nested inside the very instantiation that needs our
persistor, so naming it would be circular. The class resolves this by making the callbacks
**member templates**, and the persistor never names the HNSW type at all.

`Context` is ours to define; the class only knows it as `typename Persistor::Context`. It carries
everything the persistor is forbidden to hold:

```c
struct Vec_ctx {
  trx_t        *trx;        // the transaction the aux writes ride
  dict_table_t *aux;        // opened once per statement, MDL held
  THD          *thd;
  uint32_t      m, vec_bytes;
  dberr_t       err;        // callbacks return void; the first failure lands here
};
```

Two consequences of `void` returns. Each callback opens by short-circuiting when `ctx->err` is
already set, so the first failure sticks and the rest become no-ops. And the caller — not the
class — decides what a failure means: `insert()` runs to completion regardless (§13).

### The graph object lives in the runtime

```c
struct vec_t : Vec_runtime {       // hangs off dict_index_t::vec
  Vec_arena   arena;               // owns all node memory
  Vec_hnsw   *hnsw;                // constructed with dims/M/ef from the DD
  rw_lock_t   latch;               // serialises writers (§17)
};
```

Order matters on teardown: the graph is destroyed before the arena its nodes live in (§16).

---

## 8. How the callbacks write to the aux table

Each callback ends in exactly one row operation on the aux table, built with InnoDB's query-graph
C API — `ins_node_create` / `row_ins_step` for the insert, `row_create_update_node_for_mysql` /
`row_upd_step` for the updates, driven through `pars_complete_graph_for_exec`.

| callback | operation |
|---|---|
| `insert_cb` | insert one row: `(id, vec, base_pk, level, neighbors)` |
| `update_neighbors_cb` | update the `neighbors` column of one row, by id |
| `update_entry_point_cb` | upsert record 0, whose `base_pk` holds the entry point |

Four things about that path are not obvious, and each is load-bearing.

**The internal SQL parser is deliberately not used.** `pars_sql` / `que_eval_sql` would be far
less code, but they serialise every statement on the global `pars_mutex` — and this path runs
once per rewired neighbour on every insert.

**The neighbour update must take its own locks.** An UPDATE normally runs a search first, which
both positions the cursor and acquires the row lock. We already know the key and skip the search,
so the lock has to be taken explicitly — `lock_clust_rec_read_check_and_lock` with `LOCK_X` and
`LOCK_REC_NOT_GAP`, not the modify variant — and the cursor position stored *after* the lock, not
before.

**Field positions are record positions.** A clustered-index record is the primary key, then
`DB_TRX_ID` and `DB_ROLL_PTR`, then the rest, so every column is addressed through
`dict_col_get_clust_pos()`. The user-column ordinal reads `DB_ROLL_PTR` instead and fails
silently downstream.

**Errors go back through `row_mysql_handle_errors`.** Each operation runs in a step-retry loop:
on a lock wait the loop blocks and retries the step, on anything else it propagates. Aux writes
therefore behave like ordinary DML under contention — waits block, deadlocks pick a victim,
errors surface — because they are driven by the same machinery the server drives, not a
reimplementation of it.

An empty blob is not a NULL. Record 0 carries a zero-length `vec` and `neighbors`, and both
columns are `NOT NULL`; writing them as SQL NULL trips the record-size assertions.

---

## 9. INSERT

A user runs:

```sql
INSERT INTO t (id, v) VALUES (7, STRING_TO_VECTOR('[1,0,0,0]'));
```

**1. The server assigns a label.** Before the base row is written, the counter hands out the
next label — say 10 — and it is stamped into the row's hidden `percona_vec_aux_id` column. The row is
inserted by the ordinary InnoDB path, on the user's transaction.

**2. We start a sub-transaction and call the graph.**

```c
Context ctx{ trx_allocate_for_background(), aux, thd, DB_SUCCESS };
hnsw.insert(/*id=*/10, /*base_pk=*/7, vector_bytes, &ctx);
```

**3. The class does its work, then calls us back.** It picks a layer for the new node, searches
for its nearest neighbours, links them mutually, and prunes each affected neighbour's edge list
back to `get_Mmax(layer)` — which is `2 * M` on layer 0, not `M`. Only when all of that is
done — the graph fully mutated — does it fire the callbacks of §7, in this order:

```
insert_cb(ctx, 10, 7, [1,0,0,0], layer, nbrs)   →  INSERT INTO aux VALUES (10, 7, …)
update_neighbors_cb(ctx, 5,  nbrs_of_5)         →  UPDATE aux SET neighbors=… WHERE id=5
update_neighbors_cb(ctx, 12, nbrs_of_12)        →  UPDATE aux SET neighbors=… WHERE id=12
      … one per neighbour whose edges changed …
update_entry_point_cb(ctx, 10)                  →  upsert record 0   (only if layer is new)
```

So one INSERT produces **one aux insert plus roughly M aux updates**, each of them a row
operation on `ctx->trx` as shown in §8. Note the callbacks fire *after* all graph mutations, not
at each one, so the neighbour list we serialise is that node's final state for this insert.

**4. We commit the sub-transaction**, before the user's statement completes.

```c
if (ctx.err != DB_SUCCESS) { trx_rollback_for_mysql(ctx.trx); /* fail the statement */ }
else                         trx_commit_for_mysql(ctx.trx);
```

If any aux write failed, the first failure is in `ctx.err`, later callbacks short-circuit, the
sub-transaction rolls back and the statement fails — taking the base row with it.

There is no NULL case to handle anywhere in this design: `sql_table.cc:5236` rejects a vector
index on a nullable column, so an indexed vector always has a value.

---

## 10. UPDATE

**Changing the vector** does not edit the node. Nodes are immutable: HNSW cannot safely move a
point once its neighbours are linked to it. So a new label is minted and inserted, and the old
node is left alone.

```sql
UPDATE t SET v = STRING_TO_VECTOR('[0,1,0,0]') WHERE id = 7;
```

```
base row 7:  percona_vec_aux_id  10 → 20
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

The cost is that dead nodes accumulate; §23 covers what that means and how it is eventually
reclaimed.

---

## 12. SELECT

```sql
SELECT id FROM t
  ORDER BY DISTANCE(v, STRING_TO_VECTOR('[1,0,0,0]'), 'EUCLIDEAN')
  LIMIT 5;
```

The point of the read path is to answer this **from the index instead of scanning the table**.
Today it cannot — §29 owns that gap — and the plan is `type=ALL` plus a filesort over every row.
What follows is the shape it has to take.

### How the optimizer reaches the index

Nothing about `DISTANCE()` is special to the optimizer today: it is an ordinary scalar function,
so `ORDER BY DISTANCE(...)` is just an expression to sort by, and a sort needs all the rows. The
recognition rule has to fire on the whole shape at once:

> an `ORDER BY` over `DISTANCE(col, <constant vector>, <metric>)`, ascending, with a `LIMIT`, where
> `col` carries a vector index and the metric matches the one the index was built with.

The query vector is a `VECTOR` value, not a string: `DISTANCE()` takes three arguments and
rejects a bare literal with `ER_WRONG_ARGUMENTS`, so it is written
`STRING_TO_VECTOR('[...]')` or supplied as a parameter.

All four parts are load-bearing. A non-constant query vector cannot be handed to the graph; a
descending order asks for the *farthest* neighbours, which HNSW does not answer; a missing
`LIMIT` asks for a full ordering, which an approximate index cannot give — the class says so
directly: *"not intended to scan the entire or large part of the index as it is approximate and
likely to omit some nodes. Optimizer should avoid using this API for queries which are likely to
do so."* And a mismatched metric would rank by a distance the graph was not built for.

Cost is not what chooses it: `dict_stats_should_ignore_index()` skips vector indexes, so there is
no cardinality to compare against a scan. The rule is therefore a match, not a cost decision —
when the shape fits, the index wins. The plan it produces has `LIMIT` pushed into the access
path, and no filesort: rows arrive in distance order already.

`ef_search` has nowhere to come from yet (§31). It is a per-query property, not a per-index one,
so it needs a session variable or a hint rather than a `WITH (...)` parameter.

### How the search runs, and how it resumes

The search is **streaming**, not a single call, and that is what makes MVCC filtering affordable:

```c
void nn_search_start(NNSearchContext *ctx, const char *q, size_t batch_size,
                     size_t ef_search, PersistorContext *);
std::pair<bool, uint64_t> nn_search_next(NNSearchContext *ctx);   // false = exhausted
```

The context owns the query vector, the visited set and the discarded set. `nn_search_next()`
serves from an internal batch; when that batch runs out it refills from the discarded nodes,
descending no further and never revisiting a node it has already returned.

```
vec_search_init:   nn_search_start(ctx, q, batch_size, ef_search, &persistor_ctx)

vec_search_next:   loop:
                       (ok, base_pk) = nn_search_next(ctx)
                       !ok?                                -> end of data
                       fetch the base row by base_pk, under the reader's read view
                       not visible?                        -> continue        (check ②)
                       row.percona_vec_aux_id != node_id?  -> continue        (check ①)
                       return the row

vec_search_end:    ctx.reset()
```

**Resumption is the `continue`.** A candidate rejected by either check costs one more
`nn_search_next()` — not a re-run with a larger `ef_search`, which would restart the descent from
the entry point and re-return everything already seen. The engine therefore keeps handing rows
up until the executor has its `LIMIT` or the graph runs out, and the caller never sees the
candidates that were filtered.

The graph walk is pure memory (plus node faults, §15). The only disk access per candidate is the
base-row fetch — the same lookup any secondary index performs to return a row. §14 explains why
the two checks are what make this correct under concurrency.

### What a short result means

`{false, 0}` means the **graph** is exhausted, not that a batch ran out — the engine has already
pulled a replacement for every filtered candidate. So a result shorter than `LIMIT k` is a
legitimate answer, from an index that is approximate by construction.

It must not be repaired. An executor that notices `k` was not reached and falls back to a table
scan has turned an approximate-nearest query into an exact one, which is a different query with
a different cost — and it would do so silently, on exactly the workloads where the index was
supposed to help.

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

### When a persistor callback fails

`HNSW::insert()` runs in two phases: it rewires the in-memory graph first, then hands the result
to the persistor. `select_neighbors()` overwrites a neighbour's whole slot array in place, so by
the time a callback can fail the old shape is gone. The class offers no undo, and adding one
would mean either an undo log for every edge or deferring all mutation until persistence
succeeds — both a large change to a class we do not own.

**We accept the divergence.** On a persistor failure the aux sub-transaction rolls back as a
unit, so *disk* is left holding a self-consistent older graph, while memory holds the newer one.
The statement fails and the base row goes with it.

That is tolerable because of what the divergence can and cannot cost:

- It **cannot** cost correctness. Every candidate the search returns is resolved through
  `base_pk` under the reader's own view (§12), so a drifted graph can only propose the wrong
  *candidates*; it can never produce a row the reader is not entitled to, or a row that does not
  exist. The two skip conditions do not depend on the graph being accurate.
- It **can** cost recall. Edges present in one copy and not the other change which neighbourhood
  a search walks, so a query may miss a row it would otherwise have ranked. This is an
  approximate index; recall is already not guaranteed.
- It is **self-correcting**. Later inserts and updates rewire the same neighbourhoods and
  persist them, and a restart discards the memory side entirely and reloads from the aux.
  Neither copy accumulates error.

So the failure mode is a temporary, bounded loss of search quality on an index that is
approximate by construction — not a wrong answer. What the engine still owes the user is a
*report*: the statement must fail loudly rather than silently succeed against a graph that was
not persisted.

---

## 14. How MVCC works

There is no vector-specific visibility machinery. The graph is a **candidate generator** and
the base table is the authority: the graph proposes, the row decides.

This works because of four properties that each exist for their own reason:

| Property | Why it exists | What it also gives |
|---|---|---|
| a vector change mints a fresh label | nodes are immutable | every historical vector is a distinct node — in effect, a version |
| nodes are never removed | HNSW cannot delete safely | that version history is retained |
| `percona_vec_aux_id` is an ordinary column | it is the `FTS_DOC_ID` device | it is **versioned by undo**, so the row version a reader sees names the node that represents it |
| edges are navigation, not data | HNSW rewires freely | the path taken to a candidate never affects what may be returned |

So one shared graph serves every transaction. It is a *superset* of what any reader should see,
and each reader narrows it with its own read view. Two checks do that.

**Check ① — `row.percona_vec_aux_id == node_id`.** Consider the update from §10, and a query for the
*old* vector:

```
graph:   node 10 → [1,0,0,0], base_pk 7      (stale — that vector is gone)
         node 20 → [0,1,0,0], base_pk 7      (current)
base:    row 7 now carries percona_vec_aux_id = 20

SELECT … ORDER BY DISTANCE(v, STRING_TO_VECTOR('[1,0,0,0]'), 'EUCLIDEAN') LIMIT 1
    node 10 is an exact match → distance 0 → base_pk 7
    but row 7's vector today is [0,1,0,0], which is far from the query
```

Returning row 7 here would rank it at a distance belonging to a vector it no longer has —
placing it ahead of rows that genuinely are near the query. Not a duplicate, not a missing row:
a wrongly ordered one, with nothing to signal it. Check ① rejects it, because `20 != 10`, and
node 20 later returns row 7 at its true distance.

The same check handles a recycled primary key: if row 7 is deleted and a different row is
inserted with `id = 7`, the stale node still points at PK 7 — but the new row's `percona_vec_aux_id` is
not 10.

**Check ② — a row deleted after the reader's snapshot.** This one needs no code. The node is
still in the graph, its `base_pk` was never touched by the delete, and the fetch under an older
read view returns the pre-delete version of the row from undo — whose `percona_vec_aux_id` still reads
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

**The graph is never loaded in one go.** Without this, the first query on an index after a
restart would read every node before returning a row.

Cold start does the minimum:

```
1. read dims, M, ef_construction from the DD
2. construct an empty HNSW with exactly those parameters
3. read record 0 -> the entry point label
       absent?  -> the index is empty; stop
4. init_from_entry_point(entry_label, &ctx)      loads exactly ONE node
```

The entry point is the graph's root, so that one node is enough to start traversing. Everything
else arrives through `load_node_cb`, which reads a single aux row by primary key and hands its
four fields back:

```c
h.load_set_layer(handle, level);     // must be first: it sizes the neighbour array
h.load_set_vec(handle, vec);
h.load_set_base_pk(handle, base_pk);
h.load_node_neighbors(handle, ids);  // must be last: allocates, and creates stubs
```

The order is part of the class's contract, not a style choice.

Loading a node creates **stubs** for each of its neighbours — nodes that exist by id and hold no
data, filled the first time a traversal reaches them. A search therefore descends one path from
the entry point to layer 0, faulting roughly one node per layer plus the neighbourhood it
examines at the bottom: for ten million rows at `M = 16`, on the order of six nodes.

Three properties decide how this behaves:

- **No read view is taken.** The graph is shared by every transaction rather than being
  per-transaction state, so there is no snapshot it could belong to; it always loads the latest
  row. A node whose statement later rolls back becomes an orphan, and the read path already
  filters orphans (§14).
- **This bounds latency, not memory.** A stub already allocates its node block including vector
  space; only the neighbour array is deferred. Nothing shrinks either — a node never returns to
  the unloaded state, and the arena has no per-block free (§16).
- **The parameters must match** what the index was built with. The class states that a mismatch
  in dimensions or `M` corrupts the graph or yields wrong results, which is why they come from
  the DD and never from a default.

One implementation trap is worth naming because it fails silently: a column's position in a
clustered-index *record* is not its user-column ordinal. The record is the primary key, then
`DB_TRX_ID` and `DB_ROLL_PTR`, then the rest, so every field read goes through
`dict_col_get_clust_pos()`. Using the ordinal reads `DB_ROLL_PTR` — seven bytes where eight were
expected.

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

Two latches hang off the runtime, with deliberately different lifetimes.

**`vec_t::graph_latch` — temporary, marked `TODO REMOVE`.** The HNSW class is not thread-safe
(`hnsw.h` @todo 1) and `Vec_arena` has no synchronisation of its own, so two concurrent inserts
could be handed the same block. Every entry into the graph takes this exclusively.

It is a plain mutex rather than an rw_lock because there are no readers to share with:
`k_nn_search()` mutates the graph too, faulting unloaded stubs in through `load_node_cb` as it
traverses (§15). So searches serialise as well. That cost is the reason it is temporary — the
intended end state is that concurrent INSERTs mutate the graph concurrently, and deleting this
latch is what delivers it. Nothing above the class needs to serialise them.

**`vec_t::load_latch` — permanent, marked `KEEP`.** It guards the one-time build of the graph
from the aux. `loaded` is a plain bool, and two threads arriving on a cold index must not both
build the graph. A thread-safe HNSW does not solve that: the duplicate work is ours, above the
class. Keeping the two separate is deliberate — folded together, whoever deletes `graph_latch`
would silently delete load safety with it.

### Why the aux needs no version column

The callbacks do not interleave with the graph mutations. `insert()` rewires the whole
neighbourhood first, collecting the touched nodes, and only then walks that set calling
`update_neighbors_cb` with each node's *final* state — so a node rewired at three layers is
persisted once, correctly. There is no older-overwrites-newer race, and therefore nothing for a
per-node mutation-order stamp to protect against.

What that ordering does create is the failure gap in §13: by the time a callback can fail, the
in-memory rewire is already done and cannot be undone. That is a divergence question rather than
an ordering one, and §13 records the decision.

One consequence is worth watching once `graph_latch` goes. Two concurrent inserts that pick
overlapping neighbours will update the same aux rows from two different background
sub-transactions, so they can deadlock with no user transaction to blame. The fix, if it is ever
needed, is to buffer the writes in `Vec_ctx` and apply them in aux-id order — a consistent lock
order makes a cycle impossible. It is deliberately **not** built: with `graph_latch` held there
is no concurrency to deadlock, so the code could not be tested, and the right time to build it is
when a real deadlock appears.

---

## 17a. Memory limits

`innodb_hnsw_max_memory` bounds, in bytes, the memory held by HNSW graphs **across all tables and
all indexes**. Dynamic, so a workload blocked by it recovers without a restart; `0` means no
limit.

Every graph byte passes through `Vec_arena::allocate()`, so one atomic counter there covers
exactly the scope the variable promises, and each arena subtracts its total when destroyed.

**The refusal deliberately does not live in `allocate()`.** Returning `nullptr` there is what
`hnsw.h` turns into a `throw std::bad_alloc` — four sites, `Node::create` and `alloc_neighbors`
among them — partway through a rewire, with neighbours already relinked and no per-block free to
unwind with (§16). So the check sits at the entry to the insert, before the graph has been
touched, where failing is just a failed statement.

**It is a charge check, not a prediction.** It asks whether the budget is already spent, not
whether this insert would fit. Sizing an insert from outside the class is not possible:
`sizeof(Node)` is private, and one insert also allocates stubs for lazily loaded neighbours and a
copy of the query vector. The bound can therefore be overshot by at most what a single insert
allocates — the price of refusing before mutating rather than during.

Refusal reaches the user as `ER_OUT_OF_RESOURCES`. That required adding `DB_OUT_OF_MEMORY` to
`row_mysql_handle_errors()`, beside `DB_OUT_OF_FILE_SPACE`: it had no case there, so it fell to
the default branch and called `ib::fatal`, taking the server down instead of failing the
statement. A resource ceiling is not a corrupt engine.

---

## 18. DDL

| Operation | Effect on the index |
|---|---|
| `CREATE TABLE … KEY (v) TYPE hnsw` | adds the hidden `percona_vec_aux_id` column, creates the aux table, registers it in the DD |
| `DROP TABLE` | drops the aux table with the parent |
| `TRUNCATE TABLE` | drop and recreate — the aux is re-created empty, and the label counter restarts |
| `RENAME TABLE` | same schema: nothing to do. Cross-schema: the aux moves with the parent |
| `DROP INDEX` | drops that index's aux table; the hidden column is retained |
| `ALTER TABLE … ADD KEY (v) TYPE hnsw` | see below |
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

## 19. `ALTER TABLE … ADD KEY (v) TYPE hnsw`

Adding a vector index to a table with existing rows is performed by **table copy**, not
in-place.

```sql
ALTER TABLE t ADD KEY vk (v) TYPE hnsw WITH (M = 16);
```

The copy path already rewrites every row into a new table, and each of those rows travels the
ordinary INSERT path of §9 — a label is assigned, `hnsw.insert()` is called, callbacks populate
the aux. So the graph is built as a side effect of the copy, with no separate build phase and no
second code path to keep correct.

In-place is refused deliberately. It would have to build the graph while concurrent writers
mutate the table, and reconcile a partially built index with changes arriving behind it. The
copy is slower and obviously correct.

---

### Locking, and why LOCK=NONE is not supported

`ADD VECTOR INDEX` requires at least `LOCK=SHARED`; `LOCK=NONE` is refused. Two independent
reasons, either sufficient:

1. The rollback path (`ddl::mark_secondary_indexes`) drops an uncommitted vector index and its
   aux from the cache immediately, which is safe only if no concurrent thread holds a prebuilt
   `ins_node` entry referencing the index.
2. `vec_build_index()` scans the clustered index *after* `ddl::Builder`, outside the online
   machinery. Under `LOCK=NONE` a concurrent INSERT would land in the row log, be applied to the
   new index by `row_log_apply`, and never reach the graph — the row would be in the table and
   not in the index.

This is **FTS parity, and a deliberate non-goal rather than a gap.** Upstream describes exactly
this trade for FULLTEXT — *"we could do without a lock if the table already contains an
FTS_DOC_ID column, but in that case we would have to apply the modification log to the full-text
indexes"* — and never built it, so `ADD FULLTEXT` sets `online = false` unconditionally too.
Supporting `LOCK=NONE` here would be a deviation *beyond* FTS and would need justifying as one.

With `online = false` there is no row log for this ALTER at all, which is what makes
`vec_base_collect_rows()` correct in reading records directly: no concurrent writer can exist, so
there is nothing for a read view to hide and delete-marked records are committed deletes awaiting
purge.

## 20. Foreign keys, and why CASCADE is refused

Every rule in §9–§12 assumes DML arrives through the handler. Cascaded DML does not. When a
parent row is deleted or its key updated, InnoDB runs the child-side action *inside the engine*,
through the FK cascade nodes in `row_ins_foreign_check_on_constraint`. Those never reach
`ha_innobase::write_row` / `delete_row`, which is where every maintenance rule here hangs.

It is worth being exact about which half of that is a problem, because the two cascades differ.

**`ON DELETE CASCADE` is harmless.** A DELETE writes nothing — §11 — because the node has to
stay for read views that are still entitled to the row. A cascaded delete therefore lands in
precisely the state an ordinary delete lands in: base row gone, aux row and node retained, read
path filtering them by base-row lookup. Nothing is owed, so nothing is missed.

**`ON UPDATE CASCADE` is a real gap**, and only in one shape. §10 says a primary-key change
re-points the row's current node:

```sql
UPDATE aux SET base_pk = <new pk> WHERE id = <current label>
```

A cascade bypasses that write, so the aux row keeps the old key. The consequence is a *false
negative*, never a false positive: the node resolves to a key that is gone, so the row silently
drops out of kNN results — and if some later row takes over that key, check ① rejects it,
because its label is different. Missing, never wrong.

Narrower still: §23 restricts us to a single-column `BIGINT UNSIGNED` primary key, so a cascade
can only reach the primary key when the foreign key *is* that column — the shared-primary-key
1:1 pattern. A cascade on any ordinary foreign-key column changes neither `base_pk` nor the
vector, and needs no maintenance at all.

`ON DELETE SET NULL` is a non-issue from both directions: it cannot target the vector column,
which must be `NOT NULL`, and nulling an ordinary column touches nothing tracked here.

### What is refused today

Both cascades, on any vector-indexed table, in `mysql_prepare_create_table`. That is broader
than the defect and deliberately so for MVP — the accurate rule is *`ON UPDATE CASCADE` where
the foreign key overlaps the primary key*, which is worth adopting when cascades are supported
for real.

### What supporting them takes

FTS solves the same problem engine-side, and its shape is the one to copy. It calls
`fts_trx_add_op()` from inside the cascade handling (`row0ins.cc:1251`), gated on a per-foreign
-key predicate `foreign->is_fts_col_affected()`, and — the important part — it **queues** the
operation on the transaction rather than doing I/O inline.

Four pieces:

| | |
|---|---|
| **A per-FK predicate** | Not "does this FK touch the vector column" — a foreign key cannot be on one. Ours is "do this FK's child columns overlap the child's primary key", since that is what invalidates `base_pk`. |
| **A hook in the `DICT_FOREIGN_ON_UPDATE_CASCADE` branch** | After `row_ins_cascade_calc_update_vec` has computed the new values. The `ON DELETE CASCADE` branch needs nothing. |
| **A deferred queue, drained at commit** | This is what makes it engine work rather than a handler tweak. Our aux writes ride a sub-transaction owned at handler level; `row0ins` cannot see it, and opening aux I/O inside cascade processing would nest transactions mid-statement. Queue "re-point label L to primary key P" and drain on the sub-transaction where §13 already drains. |
| **Recursion safety** | Cascades chain through multi-level foreign-key graphs, so the queue is per affected child row, not per statement. |

It reuses the sub-transaction drain, so it belongs after the write path exists rather than
alongside it.

---

## 21. Durability and crash recovery

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
the interesting window, and the ordering rule in §22 makes it safe: the sub-transaction commits
first, so a crash in between leaves an orphan node, never a committed row without one.

---

## 22. The rules

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

## 23. Limitations

**Dead nodes accumulate.** A deleted row, a superseded vector and a rolled-back insert all leave
a node behind, and MVP never removes them. The index therefore grows with the number of
*mutations* rather than the number of rows. They are reclaimed today only by dropping the index
or rebuilding it.

Reclaiming them properly is a later phase. The rule is known — a label may be removed once no
active read view can see any row version carrying it, which is exactly the negation of the
condition check ② depends on — and it needs a record of retirement events, since one of them (a
rolled-back insert) leaves no trace anywhere else in the engine.

**Writers are serialised** until the class becomes thread-safe (§17).

**Rejected at DDL by the layer above**, independently of anything here: a vector index on a
virtual generated column, on a partitioned table, or on a temporary table. `sql_table.cc` raises
`ER_INDEX_MUST_HAVE_COMPATIBLE_COLUMN` for the first and `ER_NOT_SUPPORTED_YET` for the other
two, so none of them reach the aux code.

**`max_elements` is unread and unsettable.** `HnswParam` carries a default of 10000, the parser
never assigns it (§5), and nothing in the tree looks at it — not the HNSW class, not the arena. Whether it is a hard cap or a sizing hint is
open, and it matters here: the arena has no per-block free (§16), so a cap that is actually
enforced would need an answer for the insert that exceeds it.

**The aux is not transactional with the base table.** By design (§13). A count of aux rows will
not equal a count of base rows, and that is correct behaviour rather than corruption.

**One vector index per table**, and a single-column integer primary key.

The restriction is about the hidden `percona_vec_aux_id` column, not about the runtime — §4 already puts
the graph on `dict_index_t`, so a second index needs no structural change there. What a second
index needs is a second *label*, because check ① compares a row's label against a node's id and
each graph numbers its nodes independently. That leaves two ways forward, and they are not
equally good:

| | how it works | cost |
|---|---|---|
| a column per vector index | `percona_vec_aux_id_1`, `percona_vec_aux_id_2`, … each moving independently | one more hidden column per index; the first `ADD` is already COPY-only (§19), so adding one is free |
| one shared column, FTS-style | a single label meaning "this version of this row", every aux keying on it | an UPDATE to *one* index's vector bumps the shared label, invalidating this row's nodes in **every** vector index — each must then insert a fresh node or the row silently vanishes from its results |

FTS chose the shared column and pays that cost: a new `FTS_DOC_ID` on UPDATE re-indexes the row
in every FULLTEXT index. For vectors the cost is worse, because re-inserting a node means
re-running graph insertion — a search plus neighbour rewiring — for an index whose vector did
not change. So a column per index is the direction, and MVP simply rejects the second index
rather than half-building it.

---

# Part II — Open items

Everything known to be missing, wrong, or temporary. One section each, with the code, an example
you can run, and who owns it. Numbering continues from Part I so a section can be cited on its
own.

The GA blocker is §24.

| § | item | owner |
|---|---|---|
| 24 | `k_nn_search()` returns no node id — **GA blocker** | HNSW class |
| 25 | Persistor callbacks cannot report failure | HNSW class |
| 26 | `insert()` is not exception-safe mid-rewire | HNSW class |
| 27 | `max_elements` is neither settable nor read | HNSW class |
| 28 | No way to size an insert before it runs | HNSW class |
| 29 | There is no vector read path at all | server |
| 30 | `SELECT COUNT(*)` returned 0 — fixed here, belongs upstream | server |
| 31 | `ef_search` has nowhere to live | server |
| 32 | `graph_latch` must be deleted when thread safety lands | us |
| 33 | The cherry-picked persistor commit must be dropped | us |
| 34 | Two `innodb` suite tests fail on the base branch | not ours |
| 35 | The upstream autoinc crash window | not ours |
| 36 | Dead nodes are never reclaimed | post-MVP |
| 37 | CASCADE foreign keys are refused | post-MVP |
| 38 | One vector index per table | post-MVP |
| 39 | Aux deadlocks between sub-transactions, if they appear | post-MVP |

---

## 24. `k_nn_search()` returns no node id — GA BLOCKER

```c
// hnsw.h:289
std::vector<uint64_t> k_nn_search(const char *q, size_t k, size_t ef_search,
                                  PersistorContext *persistor_ctx = nullptr);
// :324   result[i - 1] = nearest.top().node->base_pk();
```

The result is base_pks only. Check ① (§14) is `row.percona_vec_aux_id == node_id`, and there is
no node id to compare against. Check ② alone is not enough.

**Example.** A single row updated once, from `vector_update.test`:

```sql
INSERT INTO t1 VALUES (7, STRING_TO_VECTOR('[1,0,0,0]'));   -- label 1, node 1
UPDATE t1 SET v = STRING_TO_VECTOR('[0,0,1,0]') WHERE id = 7;
```

```
count=4 max_id=3
id=1 level=0 base_pk=7 nb=2,3      <- superseded node, still holds [1,0,0,0]
id=3 level=1 base_pk=7 nb=...      <- current node, holds [0,0,1,0]
```

Row 7 now carries `percona_vec_aux_id = 3`. Two nodes name `base_pk = 7` on purpose (§10 — a
node is immutable, so a changed vector becomes a new node).

Now a search for `[1,0,0,0]`:

1. The graph walks into **node 1**, which is a perfect match for the vector row 7 *used to* have.
2. Check ② asks "is `base_pk = 7` visible to my read view?" — yes, row 7 is live and committed.
3. Row 7 is returned, ranked as if it still held `[1,0,0,0]`.
4. Check ① would have rejected it — the row says 3, the node is 1 — and cannot run.

The result is not an error, a warning, or a crash. It is a **silently wrong ranking**, and it
appears the moment anyone updates an indexed vector.

**Fix.** Return the node id (and ideally the distance, which the server otherwise recomputes per
row). `vector_update.test` already records the aux state that produces this, so the test to prove
the fix exists as soon as there is a read path to query (§29).

---

## 25. Persistor callbacks cannot report failure

```c
// hnsw.h:172
void insert(uint64_t id, uint64_t base_pk, const char *q, ...);   // returns void
```

Two distinct holes.

**`load_node_cb` has no failure path and fires inside the rewire loop.** At `hnsw.h:240`
`load_node_if_necessary(persistor_ctx, *nb)` runs while neighbour candidates are being gathered.
Our implementation reads a row from the aux table — that can fail on I/O or a corrupt row, and
there is no way to say so. The loop simply continues:

```c
load_node_if_necessary(persistor_ctx, *nb);                          // :240  may have failed
candidate_neighbors.push_back({*nb, dist(neighbor->vec(), *nb)});    // :241  measures a stub
...
select_neighbors(neighbor->vec(), candidate_neighbors, Mmax,         // :247  picks edges from it
                 neighbor->neighbors_begin(*this, l));
```

**`insert()` returns void.** A failed `insert_cb` (`:255`) or `update_neighbors_cb` (`:261`) can
set `ctx->err` so later callbacks short-circuit, but cannot stop `insert()` and cannot tell the
caller.

**Example.** The aux tablespace goes read-only between an INSERT starting and its persistor
running. `insert()` rewires the graph in memory, every aux write fails, `ctx->err` is set, and
`insert()` returns normally. We roll the sub-transaction back and fail the statement — so the
user does see *an* error here, because we check `ctx->err` ourselves. But nothing propagates from
a `load_node_cb` failure at all: that path has no error slot to set.

§13 records the decision to accept the resulting memory/disk divergence, which makes this
not-MVP-critical. What is still owed is that a failure be *reportable* rather than silent.

This is the class's own `@todo 2) Better error handling (e.g. errors from persistor callbacks,
OOMs)` — worth saying it has been hit for real rather than in theory.

---

## 26. `insert()` is not exception-safe mid-rewire

Four `throw std::bad_alloc` sites are reachable from inside the rewire loop:

| site | what it was allocating |
|---|---|
| `hnsw.h:396` | a copy of the query vector |
| `hnsw.h:840` | `Node::create` — `ALIGN_SIZE(sizeof(Node)) + ALIGN_SIZE(dims * sizeof(float))` |
| `hnsw.h:857` | `Node::create`, stub overload |
| `hnsw.h:907` | `alloc_neighbors` — `(layer + 2) * M * sizeof(Node *)` |

**Example.** `alloc_neighbors` throws for a node at layer 3 with `M = 16` while the loop at
`hnsw.h:210` has already relinked two earlier layers. The exception unwinds out of `insert()`,
through `vec_add_node()`, into InnoDB code that is not exception-safe — with the graph
half-rewired and `graph_latch` released by its `lock_guard` on the way out, so the next thread in
sees the half-rewired graph as if it were finished.

§17a keeps us off these paths for the *budget* case by refusing before `insert()` starts, but a
genuine allocator failure still reaches them.

**Question for the class owner:** is `insert()` intended to be exception-safe, and what is the
graph's state after a throw?

---

## 27. `max_elements` is neither settable nor read

```c
// vec0vec.h:39
int max_elements{10000};
```

Nothing in the tree sets it and nothing reads it. It is unreachable from SQL:

```sql
mysql> CREATE TABLE bad (id INT PRIMARY KEY, v VECTOR(4) NOT NULL,
    ->                   KEY (v) TYPE hnsw WITH (max_elements = 100));
ERROR 7038 (HY000): Illegal index construction parameter: max_elements.
```

The `WITH (...)` parser (`vec0vec.cc:161-184`) accepts `M`, `ef_construction` and
`metric`, and its `else` branch rejects everything else. So the field's 10000 is a default that
nothing can change and nothing consults.

**Why it matters here.** If it is a real cap, an insert that would exceed it needs a defined
outcome, and the arena has no per-block free (§16) to unwind with — the same problem §17a solves
by refusing before `insert()` begins. **Cap, sizing hint, or delete?**

---

## 28. No way to size an insert before it runs

`Node::create` allocates `ALIGN_SIZE(sizeof(Node)) + ALIGN_SIZE(dims * sizeof(float))` and
`alloc_neighbors` allocates `(layer + 2) * M * sizeof(Node *)`, where `layer` is
`random_layer(max_layer)` capped at `max_layer + 1` (`hnsw.h:1056`).

That cannot be computed from outside the class: `sizeof(Node)` is private, and one insert may
also allocate stubs for lazily loaded neighbours plus a copy of the query vector.

**Consequence.** §17a is a *charge* check — "is the budget already spent?" — rather than a
prediction — "would this insert fit?". So `innodb_hnsw_max_memory` can be overshot by whatever a
single insert allocates.

**Fix.** A static `HNSW::worst_case_insert_bytes(dims, M, max_layer)` would make the check exact
and remove the overshoot.

---

## 29. There is no vector read path at all

`DISTANCE()` is an ordinary scalar function — `class Item_func_vector_distance final : public
Item_real_func` (`item_strfunc.h:1313`). No optimizer code references `HA_VECTOR` or
`is_vector_index`. `handler` has no vector-search API: compare `virtual int ft_init()`
(`handler.h:6138`) and its `ft_init_ext` / `ft_read` siblings, which have no vector
equivalent.

**Example.** The query this whole design exists to serve, on a table with a vector index:

```sql
mysql> EXPLAIN SELECT id FROM t1
    ->   ORDER BY DISTANCE(v, STRING_TO_VECTOR('[1,0,0,0]'), 'EUCLIDEAN') LIMIT 1;
type=ALL   possible_keys=NULL   key=NULL   rows=2   Extra=Using filesort
```

Full table scan plus a filesort. The index is never consulted, and nothing warns you.

**Why a new API family, rather than teaching the optimizer to use the existing one.** The
handler's index API is B-tree-shaped: `index_read` takes a key and a comparison operator,
`index_next` walks in key order, `records_in_range` estimates a range. None of that can express
"the k nearest to q" — there is no key to seek, no order to walk, and no range. FTS hit exactly
this wall, which is why `ft_init` / `ft_init_ext` / `ft_read` exist as a *separate* family beside
`index_*` and `rnd_*` rather than as a mode of them.

So without such a family the index cannot be used for a nearest-neighbour query at all, and the
only correct plan is the full scan plus filesort above. Worse, the index is not merely unused —
§30 shows it is currently *chosen*, wrongly, for scans it cannot serve. Both halves are the same
work: `index_flags` must stop advertising it, and a `vec_search_*` family must start offering it.

**What it needs**, in dependency order:

1. **A handler API**, shaped like the FT family:
   and mapping straight onto the class's streaming search (§12):

   | handler | engine |
   |---|---|
   | `vec_search_init(uint index, const char *q, size_t q_len, size_t k, size_t ef_search)` | `nn_search_start()` |
   | `vec_search_next(uchar *buf)` | `nn_search_next()` in a loop, skipping rows the two MVCC checks reject |
   | `vec_search_end()` | `NNSearchContext::reset()` |

   plus a capability flag. End-of-data from `vec_search_next` means the *graph* is exhausted, not
   that a first batch ran out.
2. **Optimizer recognition** of `ORDER BY DISTANCE(col, <const>, <metric>) LIMIT k` where `col`
   carries a vector index, so the plan stops being `type=ALL` plus a filesort. Rule-based is
   enough — there is no cardinality to cost against, since `dict_stats_should_ignore_index()`
   skips vector indexes. The class asks to be kept away from queries that would walk most of the
   index, so a small `LIMIT` is the shape it serves.
3. **An executor access path** that drives those and consumes rows already in distance order, so
   the filesort is elided rather than run.

The engine side (streaming search, both checks, pulling another candidate when one is filtered
out) is P4b in
`PLAN-vec-hnsw-aux-sub-trx.md`, along with the contract between the halves. Its sharpest clause,
and the one most likely to be got wrong: **the engine may return fewer than `k` rows even when
more matching rows exist**, because the index is approximate and MVCC filters candidates *after*
the graph has chosen them. The engine pulls another candidate for each one filtered out, so a
short result means the graph is genuinely exhausted — not that it gave up early. An optimizer
that patches such a result with a table scan turns an approximate-nearest query into an exact
one, which is a different query.

---

## 30. `SELECT COUNT(*)` returned 0 — fixed, and why it happened

**Fixed on this branch.** Recorded because the shape of the mistake is worth keeping, and because
the fix belongs upstream in PR #5987 rather than here.

`ha_innobase::index_flags()` opens with a FULLTEXT branch returning `0` — advertise nothing, so
the optimizer can never choose it for an ordinary scan. There was no vector equivalent, so a
vector index fell through to the default set:

```
HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN
```

`HA_KEYREAD_ONLY` is the damaging one: it makes the index a covering-index candidate, and
`sql_table.cc:5241` gives a vector key part a dummy length of 1, so it also looked like the
*narrowest* index on the table — the most attractive choice for a count.

```sql
mysql> SELECT COUNT(*) FROM t1;                 -- two rows in the table
0
mysql> EXPLAIN SELECT COUNT(*) FROM t1;
type=index   key=v   key_len=1   Extra=Using index
```

The optimizer scanned a B-tree that does not exist — `DICT_VECTOR` is in the no-real-tree family
beside `DICT_FTS`, and the root page may be `FIL_NULL` — and the count came back zero with no
error and no warning. Reachable without writing a single vector query; existing tests hid it
behind `FORCE INDEX (PRIMARY)`.

The fix is the one line FULLTEXT already uses. One visible consequence: `SHOW INDEX` now reports
`Collation = NULL` for a vector index rather than `A`, which is the loss of `HA_READ_ORDER` and
more truthful than what it replaced — there is no ascending order to report. FULLTEXT reports
`NULL` there for the same reason. Forcing the index is accepted and simply has no effect: with
nothing advertised it cannot serve a scan, so the optimizer plans around it.

The general rule this leaves behind, and the reason §29 is the other half of the same work: **an
index must not advertise a capability it cannot honour.** Until a `vec_search_*` family exists,
a vector index is unreadable, and saying otherwise is what produced wrong results.

---

## 31. `ef_search` has nowhere to live

`k_nn_search()` takes `ef_search` as a parameter, and nothing in the server can supply it.
`ef_construction` is parsed from `WITH (...)` but applies at build time only; there is no search
width anywhere.

**Example.**

```sql
mysql> CREATE TABLE t (id INT PRIMARY KEY, v VECTOR(4) NOT NULL,
    ->                 KEY (v) TYPE hnsw WITH (ef_search = 100));
ERROR 7038 (HY000): Illegal index construction parameter: ef_search.
```

And correctly so — search width is a per-query property, not a per-index one. It needs a session
sysvar (`innodb_hnsw_ef_search`) or a query hint. Until then the read path has to hard-code one.

---

## 32. `graph_latch` must be deleted when thread safety lands

Marked `TODO REMOVE` in the source, on the field and on every `lock_guard`. Deleting it is what
lets concurrent INSERTs mutate the graph concurrently, which is the intended end state (§17).

**Do not delete `vec_t::load_latch` with it.** That one is marked `KEEP` and is ours: it stops
two threads on a cold index from both building the graph from the aux, which a thread-safe HNSW
does not address. They are separate fields for exactly this reason.

**Example of the mistake to avoid** — the first version of this change had one guard covering
both, so deleting it would silently have removed load safety too:

```c
std::lock_guard<std::mutex> graph_guard(vec->graph_latch);   // TODO REMOVE
if (!vec->loaded) { vec_runtime_load(...); }                 // ... but this must stay guarded
vec->hnsw->insert(...);
```

Removing the latch is also the point at which §39 becomes reachable.

---

## 33. The cherry-picked persistor commit must be dropped

`Basic HNSW class (phase 2 — support for persistence and loading) [part 1]` is carried on this
branch from `vector-mvp-11267` so that P2 could be built before it landed. Drop it once PS-11267
is in `vector-mvp`, or the branch will carry a duplicate.

---

## 34. Two `innodb` suite tests fail on the base branch

`innodb.innodb_stats` and `innodb.use_latest_stats`. PR #5987 added an `INDEX_OPTIONS` column to
`INFORMATION_SCHEMA.STATISTICS` (`sql/dd/impl/system_views/statistics.cc`) without updating the
two result files.

**Example** — every diff line is one of these two shapes:

```
-... IS_VISIBLE  EXPRESSION
+... IS_VISIBLE  EXPRESSION  INDEX_OPTIONS
-def test t1 1 test a 1 a A 1 NULL NULL YES BTREE   YES NULL
+def test t1 1 test a 1 a A 1 NULL NULL YES BTREE   YES NULL  flags=0;
```

Nothing on this branch touches `sql/dd/` or those results, and the failure diff hashes identically
before and after our changes. Belongs in Martin's PR.

---

## 35. The upstream autoinc crash window

Written up in `PS-autoinc-persist-crash-window.md`. Upstream advances the persisted autoinc
watermark *before* the redo record enters the mtr, so a racing thread sees the watermark already
advanced, skips logging, and a committed value can be reissued after a crash.

Our counter deliberately does not copy that: `dict_table_vec_next_id_persisted_advance()` is
called *after* `mtr.commit()`. The deviation is intentional and documented; the upstream autoinc
bug it avoids is still unfiled.

---

## 36. Dead nodes are never reclaimed

Deleted rows (§11), superseded vectors (§10) and rolled-back inserts (§13) all leave nodes
behind, so the index grows with the number of **mutations** rather than the number of rows.

**Example.** Two rows, one of them updated three times:

```sql
INSERT INTO t1 VALUES (7,...),(8,...);
UPDATE t1 SET v = ... WHERE id = 7;   -- three times, different vectors
SELECT COUNT(*) FROM t1 FORCE INDEX (PRIMARY);   -- 2
```

```
nodes=5  distinct_ids=5  distinct_base_pks=2  dup_base_pks=3
```

Five nodes for two live rows. Three of them are dead and will never be removed; the only
reclamation today is dropping or rebuilding the index.

The retirement rule is known — a label may be removed once no active read view can see any row
version carrying it, the exact negation of what check ② depends on. It needs a durable record of
retirement events, because a rolled-back insert leaves no trace anywhere else in the engine.

---

## 37. CASCADE foreign keys are refused

**Example.**

```sql
mysql> CREATE TABLE ch (id BIGINT UNSIGNED PRIMARY KEY, p BIGINT UNSIGNED,
    ->                  v VECTOR(4) NOT NULL, KEY (v) TYPE hnsw WITH (M=4),
    ->                  FOREIGN KEY (p) REFERENCES par(id) ON UPDATE CASCADE);
ERROR 1235 (42000): This version of MySQL doesn't yet support
                    'vector indexes on tables with a CASCADE foreign key'
```

§20 gives the reasoning and what support would take. The refusal is deliberately broad: `ON
DELETE CASCADE` is in fact harmless, because DELETE writes nothing to the aux. Narrowing it to
`ON UPDATE CASCADE` where the foreign key overlaps the primary key — the only case that actually
moves a `base_pk` — is the follow-up.

---

## 38. One vector index per table

**Example.**

```sql
mysql> ALTER TABLE t1 ADD KEY k2 (v) TYPE hnsw WITH (M = 4);
ERROR 1235 (42000): This version of MySQL doesn't yet support
                    'multiple vector indexes on a single table'
```

The restriction is about the hidden label column, not the runtime — §4 already puts the graph on
`dict_index_t`, so a second index needs no structural change there. What a second index needs is a
second *label*, because check ① compares a row's label against a node id and each graph numbers
its nodes independently. §23 sets out the two ways forward and why a hidden column per index beats
the shared FTS-style column for vectors.

---

## 39. Aux deadlocks between sub-transactions, if they appear

Not reachable today, and deliberately not fixed.

Once §32 removes `graph_latch`, two concurrent inserts that pick overlapping neighbours will
update the same aux rows from two different background sub-transactions. The order they do it in
is chosen by `std::set<Node *>` iteration (`hnsw.h:208`, `:257`) — that is, by arena address:
arbitrary, and not reproducible between processes.

**Example of the cycle that becomes possible.** Two inserts both rewire nodes 5 and 9:

```
sub-trx A:  UPDATE aux SET neighbors=... WHERE id=9   (granted)
sub-trx B:  UPDATE aux SET neighbors=... WHERE id=5   (granted)
sub-trx A:  UPDATE aux SET neighbors=... WHERE id=5   (waits on B)
sub-trx B:  UPDATE aux SET neighbors=... WHERE id=9   (waits on A)  -> deadlock
```

Neither has a user transaction to retry it.

**The fix, if it is ever needed.** Buffer `(id, neighbour blob)` pairs in `Vec_ctx` instead of
writing from inside the callbacks, then sort by aux id and apply after `insert()` returns. A
consistent global lock order makes a cycle impossible. The blob has to be copied regardless,
since `NeighborIdRange` is valid only for the duration of the callback.

It does **not** reduce contention — both transactions still want the same row and one still waits
— but converting a deadlock into a wait is the entire point.

### Why the answer is ordering, not a retry count

The obvious alternative is to retry the failed sub-transaction a few times. It is worth writing
down why that does not work here, because it is the first thing anyone will reach for.

What the aux writes do today, through `row_mysql_handle_errors` (§8):

| | behaviour |
|---|---|
| `DB_LOCK_WAIT` | suspends, returns `true`, and the step is retried — unbounded in count, each wait bounded by `innodb_lock_wait_timeout` |
| `DB_LOCK_WAIT_TIMEOUT` | rolls back to the statement savepoint, propagates. No retry |
| `DB_DEADLOCK` | rolls back the **entire sub-transaction**, propagates. No retry |

Lock waits are therefore already handled the way ordinary DML handles them. A deadlock is not,
and cannot be, for two structural reasons:

- **The sub-transaction is already gone.** `row_mysql_handle_errors` calls
  `trx_rollback_to_savepoint(trx, nullptr)` for `DB_DEADLOCK` before returning, so there is
  nothing to resume. A retry means re-issuing every aux write for that `insert()`.
- **The data to re-issue no longer exists.** The callbacks have fired and returned, and
  `NeighborIdRange` is valid only for the duration of a callback, so each one serialised into a
  buffer that died with the call. `insert()` returns `void` and cannot be re-run either — the
  graph is already rewired in memory (§13).

So a retry is only possible *after* the buffering described above exists. And once it does,
sorting by aux id removes the cycle outright, which is strictly better: a retry still discards
the work and can still lose the next time.

Retrying a lock-wait *timeout* is mechanically possible — only the statement savepoint was rolled
back — and still undesirable. At the default 50-second timeout, two retries mean the user's
statement stalls for 150 seconds before failing, where propagating fails it in 50.

**Why it is not built.** With `graph_latch` held there is no concurrency to deadlock, so the code
could not be tested; it would be a mechanism justified by a scenario the code cannot reach. Build
it when a real deadlock appears.
