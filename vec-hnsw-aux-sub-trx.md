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
                              vec_hnsw_<tid>_<iid>    on disk
```

**`dict_index_t::vec`** is a pointer to the runtime. It is `nullptr` until something first opens
the index, and it owns the `HNSW` object, the arena its nodes are allocated from, the persistor,
and the index parameters read back from the DD.

### Why the runtime hangs off the index, not the table

Everything the runtime holds is a property of one index, not of the table: the dimension, the
distance metric, `M` and `ef_construction`, the entry point, the label space, the graph itself.
Two vector indexes on the same table share none of it. The aux table name settles the argument
on its own — `vec_hnsw_<table_id>_<index_id>` is keyed by index, so a table-level pointer would
have needed a lookup to find the right graph anyway. Lifetime lines up too: `DROP INDEX`
destroys exactly one runtime, and `dict_mem_index_free()` is the natural place to release it.

FTS is the obvious counter-example, and it is worth being precise about why it differs rather
than copying it. InnoDB splits FTS aux tables into two kinds — the enum says so outright:

```c
enum fts_table_type_t {
  FTS_INDEX_TABLE,   // specific to a particular FTS index on a table
  FTS_COMMON_TABLE,  // common for all FTS indexes on a table
  ...
};
```

`FTS_COMMON_TABLE` covers `DELETED`, `BEING_DELETED`, `CONFIG` and their caches — one set per
*table*, shared by every FULLTEXT index on it. And `fts_t` holds exactly the matching things:

```c
struct fts_t {
  fts_cache_t *cache;      // one shared in-memory cache
  ulint        doc_col;    // the FTS_DOC_ID column number in the clustered index
  ib_vector_t *indexes;    // ...and a LIST of the FULLTEXT indexes
  ...
};
```

So `table->fts` is not "one index's runtime parked at table scope". It is the genuinely
table-scoped state — one shared `FTS_DOC_ID` column, one doc-id counter, one delete list —
*plus a list* of the indexes that share it. The per-index state lives in the six
`FTS_INDEX_TABLE` aux tables, keyed by index id.

Vector has no equivalent shared machinery. There is no shared cache, no shared delete list, and
no shared configuration. The one thing that is table-scoped is the hidden `vec_idx_id` column
below — and that is precisely the thing that decides how many vector indexes a table may have,
which is why it gets its own treatment in §22 rather than being buried in a pointer's
placement.

Counting the dereferences outside `fts0*` makes the split concrete: `->fts->cache` appears 109
times and `->fts->doc_col` 22, against 12 for `->fts->indexes` — most of which are
`ib_vector_getp(…, 0)` or an is-empty test. The list is a convenience; the shared cache and the
shared doc column are the reason the struct is where it is.

Vector has no equivalent shared machinery, and going per-index is also the *smaller* change.
Everything a table-level container would buy already exists more cheaply: the O(1) "does this
table have one" test is a flag bit in FTS (`DICT_TF2_FTS`), not the pointer; iterating vector
indexes is `table->indexes` filtered by the already-existing `dict_index_t::is_vector()`; and
`dict_mem_index_free()` is already the release point. A container would instead add a struct, an
allocator, a free path in `dict_mem_table_free`, and a list to keep in sync across every
ADD/DROP INDEX — a sync upstream already has to assert about, in `fts_check_cached_index()`.

One constraint follows from where the pointer lands. `dict_index_t` is never constructed or
destructed — the memory is zeroed and `dict_mem_fill_index_struct()` acts as the constructor, so
`dict_mem_index_free()` has to "destruct" members by hand. `Vec_runtime *vec` must therefore be a
raw pointer, zero-initialised by that memset, assigned on open and released in
`dict_mem_index_free()`, exactly as `destroy_fields_array()` does today. No smart pointer, no
non-POD member.

The dictionary already agrees with this placement. `DICT_VECTOR` is a bit in
`dict_index_t::type` — the index-type family beside `DICT_FTS` and `DICT_SPATIAL` — not a table
flag; there is no `DICT_TF2_VECTOR`, and no table-level "has a vector index" test exists anywhere
in the tree. Six of its call sites use the paired form `(DICT_FTS | DICT_VECTOR)`, which is the
*this index has no real B-tree* family, and `dict0dd.cc:2989` states the invariant directly:

```c
ut_ad(!!(type & (DICT_FTS | DICT_VECTOR)) == (n_uniq == 0));
```

with the root page permitted to be `FIL_NULL`. A vector `dict_index_t` is therefore a dictionary
placeholder with no key and possibly no tree — which is exactly why it has room to own the
runtime, and why nothing else in that struct competes for the role.

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

Upstream already has the beginning of this seam, and it is not a vtable. `vec0vec.h` defines
the parameter side as a variant:

```c
struct HnswParam {
  int M{25};  int max_elements{10000};  int ef_construction{200};
  std::string_view metric{"euclidean"};
};
using VectorIndexParam = std::variant<std::monostate, HnswParam>;
```

Adding a second index type there is adding an alternative to the variant. The registry above is
about *behaviour* dispatch rather than parameters, so the two are not in conflict — but the
parameter half must be `VectorIndexParam`, not a parallel structure of our own. `parse_options()`
already fills it and currently has **no callers**; only `validate_options()` is wired, at
`ha_innodb.cc:4821`. Calling `parse_options` and storing the result on the runtime is our work.

Two rules keep the seam honest:

- **Implementations are stateless.** All per-index state lives in the runtime hanging off
  `dict_index_t`, so there is no object lifetime to manage and no per-index singleton.
- **The runtime is typed as a generic base.** `dict_index_t::vec` is a `Vec_runtime *`;
  hnsw's own struct derives from it, and only the implementation that allocated a runtime may
  interpret the subtype. The downcast is file-local to the hnsw implementation, so the
  compiler enforces the boundary.

Dispatch has exactly two sources, and neither guesses:

| situation | resolved by |
|---|---|
| a runtime is open | `index->vec->impl` — an open runtime records the implementation that created it |
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

### What the functions must look like

There is one wrinkle. The neighbour range the class passes is `HNSW<A,P>::NeighborIdRange` — a
type nested inside the very instantiation that needs our persistor. Naming it in our signature
would be circular. The class resolves this by making the callbacks **member templates**, so the
persistor never names the HNSW type at all:

```c
struct Vec_persistor {
  struct Context {
    trx_t        *trx;    // the sub-transaction the aux writes ride
    dict_table_t *aux;    // opened once per statement, MDL held
    THD          *thd;
    dberr_t       err;    // callbacks return void — the first failure lands here
  };

  // NeighborIds is deduced as HNSW<...>::NeighborIdRange — a range of uint64_t,
  // 0 meaning an empty slot. Templated to avoid naming the instantiation.
  template <typename NeighborIds>
  void insert_cb(Context *ctx, uint64_t id, uint64_t base_pk, const char *q,
                 uint8_t layer, NeighborIds nbrs) {
    if (ctx->err != DB_SUCCESS) return;               // an earlier callback failed
    ctx->err = vec_aux_insert(ctx, id, base_pk, q, layer, serialize(nbrs));
  }

  template <typename NeighborIds>
  void update_neighbors_cb(Context *ctx, uint64_t id, NeighborIds nbrs) {
    if (ctx->err != DB_SUCCESS) return;
    ctx->err = vec_aux_update_neighbors(ctx, id, serialize(nbrs));
  }

  // no template needed — plain scalar arguments
  void update_entry_point_cb(Context *ctx, uint64_t id) {
    if (ctx->err != DB_SUCCESS) return;
    dberr_t e = vec_aux_update_entry_point(ctx, id);  // UPDATE record 0
    if (e == DB_RECORD_NOT_FOUND)                     // first ever — create it
      e = vec_aux_insert_entry_point(ctx, id);
    ctx->err = e;
  }

  template <typename Hnsw>
  void load_node_cb(Context *ctx, Hnsw &h, typename Hnsw::LoadNodeHandle handle) {
    if (ctx->err != DB_SUCCESS) return;
    ctx->err = vec_aux_load_node(ctx, h, handle);
  }
};

using Vec_hnsw = HNSW<Vec_arena, Vec_persistor>;
```

Being member templates has one practical consequence: their bodies must live in a header. So
each is a **thin shim that forwards immediately to a plain function** — `vec_aux_insert`,
`vec_aux_update_neighbors`, `vec_aux_load_node` — which are ordinary non-template functions
compiled in a `.cc` file. The shim does only two things: short-circuit if an earlier callback
already failed, and convert the neighbour range into the on-disk blob. Everything else is §8.

That `if (ctx->err != DB_SUCCESS) return;` in every shim is how a `void` callback reports
failure. The class keeps going through the rest of its callbacks; they all no-op; and the caller
inspects `ctx->err` once `insert()` returns.

The `NeighborIds` range and the vector pointer `q` are **valid only for the duration of the
call** — the class is explicit about that — so anything we keep must be copied before returning.
We copy into the row we are about to write, so this falls out naturally.

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

Every callback ends in one row operation, built with InnoDB's query-graph C API.

```c
static dberr_t aux_insert(Context *ctx, uint64_t id, uint64_t base_pk,
                          const char *q, uint8_t layer,
                          const std::vector<byte> &nbrs) {
  mem_heap_t *heap = mem_heap_create(1024, UT_LOCATION_HERE);
  ins_node_t *node = ins_node_create(INS_DIRECT, ctx->aux, heap);

  dtuple_t *row = dtuple_create(heap, ctx->aux->get_n_cols());
  dict_table_copy_types(row, ctx->aux);
  ins_node_set_new_row(node, row);

  set_int (row, COL_ID,        id,                        heap);
  set_int (row, COL_BASE_PK,   base_pk,                   heap);
  set_blob(row, COL_VEC,       q, dims * sizeof(float),   heap);
  set_int (row, COL_LEVEL,     layer,                     heap);
  set_blob(row, COL_NEIGHBORS, nbrs.data(), nbrs.size(),  heap);

  // builds the query-graph fork only — despite the name, parses nothing
  que_thr_t *thr = pars_complete_graph_for_exec(node, ctx->trx, heap, nullptr);
  node->state = INS_NODE_SET_IX_LOCK;

  dberr_t err = run_dml(thr, node, ctx->trx, row_ins_step);
  mem_heap_free(heap);
  return err;
}
```

### `vec_aux_update_neighbors`

An UPDATE normally runs a search first, which positions the cursor *and* takes the locks. We
already know the key, so we skip the search — and must therefore take those locks ourselves.

```c
static dberr_t vec_aux_update_neighbors(Context *ctx, uint64_t id,
                                        const std::vector<byte> &nbrs) {
  mem_heap_t   *heap  = mem_heap_create(1024, UT_LOCATION_HERE);
  dict_index_t *clust = ctx->aux->first_index();
  upd_node_t   *node  = row_create_update_node_for_mysql(ctx->aux, heap);

  dtuple_t *ref = dtuple_create(heap, 1);          // the PK we are updating
  dict_index_copy_types(ref, clust, 1);
  byte key[8];
  mach_write_to_8(key, id);
  dfield_set_data(dtuple_get_nth_field(ref, 0), key, sizeof key);

  upd_t       *upd = node->update;                 // a one-field update
  upd_field_t *uf  = upd_get_nth_field(upd, 0);
  upd_field_set_field_no(
      uf, dict_col_get_clust_pos(ctx->aux->get_col(COL_NEIGHBORS), clust), clust);
  dfield_set_data(&uf->new_val, nbrs.data(), nbrs.size());
  upd->n_fields = 1;

  que_thr_t *thr = pars_complete_graph_for_exec(node, ctx->trx, heap, nullptr);
  que_thr_move_to_run_state_for_mysql(thr, ctx->trx);

  mtr_t mtr;
  mtr_start(&mtr);
  node->pcur->open_no_init(clust, ref, PAGE_CUR_LE, BTR_SEARCH_LEAF, 0, &mtr,
                           UT_LOCATION_HERE);

  const rec_t *rec = node->pcur->get_rec();
  if (!page_rec_is_user_rec(rec) ||
      node->pcur->get_low_match() < dict_index_get_n_unique(clust)) {
    mtr_commit(&mtr);  mem_heap_free(heap);
    return DB_RECORD_NOT_FOUND;
  }

  dberr_t err = lock_table(0, ctx->aux, LOCK_IX, thr);            // table IX
  if (err == DB_SUCCESS)
    err = lock_clust_rec_read_check_and_lock(                     // record X
        {}, node->pcur->get_block(), rec, clust, offsets,
        SELECT_ORDINARY, LOCK_X, LOCK_REC_NOT_GAP, thr);

  if (err == DB_SUCCESS) node->pcur->store_position(&mtr);        // AFTER the lock
  mtr_commit(&mtr);

  if (err == DB_SUCCESS) err = run_dml(thr, node, ctx->trx, row_upd_step);
  mem_heap_free(heap);
  return err;
}
```

Two details in there are not obvious and were each found the hard way.

The record lock must be taken with `lock_clust_rec_read_check_and_lock(… LOCK_X,
LOCK_REC_NOT_GAP …)` — the lock a searched UPDATE's *read* would have acquired. Reaching for
`lock_clust_rec_modify_check_and_lock` instead trips an assertion inside `row_upd_clust_step`,
which expects the transaction to already hold an explicit record X lock.

And `store_position()` must come **after** the lock succeeds. Storing it first and then waiting
on the lock leaves the cursor pointing at a position that may have moved by the time the wait
returns.

### `vec_aux_update_entry_point`

The same positioned update, against record 0 and column `base_pk` instead — plus the insert
fallback that makes it an upsert:

```c
void update_entry_point_cb(Context *ctx, uint64_t id) {
  if (ctx->err != DB_SUCCESS) return;
  dberr_t e = vec_aux_update_col(ctx, /*id=*/0, COL_BASE_PK, id);
  if (e == DB_RECORD_NOT_FOUND)                    // no record 0 yet — first node
    e = vec_aux_insert_meta(ctx, /*entry=*/id);    // vec_aux_insert with empty blobs
  ctx->err = e;
}
```

Update-then-insert rather than insert-then-catch-duplicate: after the first write, update is the
common case, and this fires at most a couple of hundred times over an index's life anyway.

Both share the run loop that `row0mysql.cc` uses for user DML:

```c
static dberr_t run_dml(que_thr_t *thr, void *node, trx_t *trx, step_fn step) {
  trx_savept_t savept = trx_savept_take(trx);
  que_thr_move_to_run_state_for_mysql(thr, trx);
  for (;;) {
    thr->run_node = thr->prev_node = node;
    step(thr);                                   // row_ins_step / row_upd_step
    dberr_t err = trx->error_state;
    if (err == DB_SUCCESS) return DB_SUCCESS;

    que_thr_stop_for_mysql(thr);
    thr->lock_state = QUE_THR_LOCK_ROW;
    bool was_lock_wait = row_mysql_handle_errors(&err, trx, thr, &savept);
    thr->lock_state = QUE_THR_LOCK_NOLOCK;
    if (!was_lock_wait) return err;              // real error — give up
  }                                              // lock wait — retry the step
}
```

That loop is why aux writes behave like ordinary DML under contention: lock waits block and
retry, deadlocks pick a victim, and errors propagate. We get all of it by driving the same
machinery the server drives, rather than reimplementing any of it.

**InnoDB's internal SQL parser is deliberately not used.** `pars_sql` / `que_eval_sql` would be
far less code, but they serialise every statement on the global `pars_mutex` — and this path
runs once per rewired neighbour on every insert.

**The transaction is a sub-transaction, not the user's.** §13 explains why, and §21 states the
rules that make it safe.

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
back to `M`. Only when all of that is done — the graph fully mutated — does it fire the
callbacks of §7, in this order:

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

The read behind that callback is a single point lookup:

```c
static dberr_t vec_aux_load_node(Context *ctx, Vec_hnsw &h,
                                 Vec_hnsw::LoadNodeHandle handle) {
  const uint64_t id    = h.load_node_id(handle);
  dict_index_t  *clust = ctx->aux->first_index();
  mem_heap_t    *heap  = mem_heap_create(512, UT_LOCATION_HERE);

  dtuple_t *ref = dtuple_create(heap, 1);
  dict_index_copy_types(ref, clust, 1);
  byte key[8];
  mach_write_to_8(key, id);
  dfield_set_data(dtuple_get_nth_field(ref, 0), key, sizeof key);

  mtr_t mtr;
  mtr_start(&mtr);
  btr_pcur_t pcur;
  pcur.open_no_init(clust, ref, PAGE_CUR_LE, BTR_SEARCH_LEAF, 0, &mtr,
                    UT_LOCATION_HERE);

  const rec_t *rec = pcur.get_rec();
  if (!page_rec_is_user_rec(rec) ||
      pcur.get_low_match() < dict_index_get_n_unique(clust)) {
    mtr_commit(&mtr);  mem_heap_free(heap);
    return DB_RECORD_NOT_FOUND;                    // no such node
  }

  ulint *offsets = rec_get_offsets(rec, clust, nullptr, ULINT_UNDEFINED,
                                   UT_LOCATION_HERE, &heap);

  // field positions in the RECORD, not user-column ordinals
  const ulint p_pk  = dict_col_get_clust_pos(ctx->aux->get_col(COL_BASE_PK),   clust);
  const ulint p_vec = dict_col_get_clust_pos(ctx->aux->get_col(COL_VEC),       clust);
  const ulint p_lvl = dict_col_get_clust_pos(ctx->aux->get_col(COL_LEVEL),     clust);
  const ulint p_nb  = dict_col_get_clust_pos(ctx->aux->get_col(COL_NEIGHBORS), clust);

  h.load_set_layer  (handle, read_u8 (rec, offsets, p_lvl));
  h.load_set_vec    (handle, read_blob(rec, offsets, p_vec));
  h.load_set_base_pk(handle, read_u64(rec, offsets, p_pk));
  h.load_node_neighbors(handle, deserialize_ids(read_blob(rec, offsets, p_nb)));

  mtr_commit(&mtr);  mem_heap_free(heap);
  return DB_SUCCESS;
}
```

The `load_set_*` calls must happen **in that order**, with `load_node_neighbors` last — it
allocates the neighbour storage, which is sized from the layer set by the first call.

A trap worth naming: a column's position in a clustered-index *record* is not its user-column
ordinal. The record is the primary key, then `DB_TRX_ID` and `DB_ROLL_PTR`, then everything
else — so `dict_col_get_clust_pos()` is required. Using the ordinal directly reads
`DB_ROLL_PTR`, which is seven bytes rather than eight and fails silently downstream.

**No read view is taken.** The graph is shared by every transaction rather than being
per-transaction state, so there is no snapshot for it to belong to; it always loads the latest
row. If a node loaded this way turns out to belong to a statement that later rolls back, it
becomes an orphan — and the read path filters orphans already (§14).

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

**Rejected at DDL by the layer above**, independently of anything here: a vector index on a
virtual generated column, on a partitioned table, or on a temporary table. `sql_table.cc` raises
`ER_INDEX_MUST_HAVE_COMPATIBLE_COLUMN` for the first and `ER_NOT_SUPPORTED_YET` for the other
two, so none of them reach the aux code.

**`max_elements` is unread.** `HnswParam` carries a default of 10000 and nothing in the tree
looks at it — not the HNSW class, not the arena. Whether it is a hard cap or a sizing hint is
open, and it matters here: the arena has no per-block free (§16), so a cap that is actually
enforced would need an answer for the insert that exceeds it.

**The aux is not transactional with the base table.** By design (§13). A count of aux rows will
not equal a count of base rows, and that is correct behaviour rather than corruption.

**One vector index per table**, and a single-column integer primary key.

The restriction is about the hidden `vec_idx_id` column, not about the runtime — §4 already puts
the graph on `dict_index_t`, so a second index needs no structural change there. What a second
index needs is a second *label*, because check ① compares a row's label against a node's id and
each graph numbers its nodes independently. That leaves two ways forward, and they are not
equally good:

| | how it works | cost |
|---|---|---|
| a column per vector index | `vec_idx_id_1`, `vec_idx_id_2`, … each moving independently | one more hidden column per index; `ADD VECTOR KEY` is already COPY-only (§19), so adding one is free |
| one shared column, FTS-style | a single label meaning "this version of this row", every aux keying on it | an UPDATE to *one* index's vector bumps the shared label, invalidating this row's nodes in **every** vector index — each must then insert a fresh node or the row silently vanishes from its results |

FTS chose the shared column and pays that cost: a new `FTS_DOC_ID` on UPDATE re-indexes the row
in every FULLTEXT index. For vectors the cost is worse, because re-inserting a node means
re-running graph insertion — a search plus neighbour rewiring — for an index whose vector did
not change. So a column per index is the direction, and MVP simply rejects the second index
rather than half-building it.
