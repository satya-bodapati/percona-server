# Implementation plan — `vec-hnsw-aux-sub-trx`

Branch base: `030d0c6101f` (head of PR#5987, `percona-mhansson/vector-mvp-syntax`).
Design: [`vec-hnsw-aux-sub-trx.md`](vec-hnsw-aux-sub-trx.md).

The pre-rebase branch (`vec-hnsw-aux-original`) carried a worked, tested DDL series against
an older base and against **hnswlib**. The new base has moved on in both respects. This file
records what survives that move, verified against code rather than commit messages.

---

## Obsolescence audit

| Old commit | Verdict | Evidence |
|---|---|---|
| `a6b53c0e8b9` — carry `WITH(...)` across ALTER | **obsolete** | `sql_table.cc:16391-16395`, inside `prepare_fields_and_keys()`, already copies `vector_index_type` and `vector_construction_params` from the old `KEY` alongside `block_size` / `parser_name` / `comment`. |
| `9ca223e5214` — fence-posts | **half obsolete** | Partitioned rejection is upstream (`sql_table.cc:8794`), and upstream adds a temporary-table rejection we never had (`:8800`). Both use `ER_NOT_SUPPORTED_YET`, so our `ER_VECTOR_INDEX_WITH_PARTITIONED_TABLE` is dead. The **CASCADE foreign-key** guard and its `ER_VECTOR_INDEX_WITH_CASCADING_FK` are still needed — no such guard exists in the new base. |
| `960c87f69d6`, `c60124e26c6` — block DISCARD/IMPORT | **still needed** | No vector guard on the discard/import path in the new base. |
| `630de8dd18a` — ADD VECTOR INDEX is COPY-only | **still needed** | The only vector gate in `check_if_supported_inplace_alter` is at `handler0alter.cc:10213`, inside **`ha_innopart::`** (starts `:10190`) — the partitioned handler, which is dead code for us now. `ha_innobase::check_if_supported_inplace_alter` (`:968`) has no vector handling at all. |
| `7abcbd071c1` — block INSTANT / BULK / native rebuild | **still needed** | No `HA_VECTOR` gate on those paths. |
| `4714dd62ba0`, `7af411bd8d0` — `percona_vec_aux_id` populate + rebuild stamp | **still needed** | The column does not exist upstream. |
| `b355205cad1` — aux module skeleton | **needs adaptation** | Column shape changed: `row_ref VARBINARY(3072)` → `base_pk BIGINT UNSIGNED`. `Vec_index_type` must be defined locally (no `vec0index.h` on this branch). |
| `60d68783de8` — `innodb_hnsw_max_memory` | **does not port** | See below. |

### Free from the new base

Plumbing we previously had to write, now upstream:

- `DICT_VECTOR` + `dict_index_t::is_vector()`; `DICT_IT_BITS` widened 10 → 11 to fit it
- `handler0alter.cc:2759` builds the `DICT_VECTOR` index def during ALTER
- `handler0alter.cc:7393` skips vector indexes in drop-time stats
- `ha_innodb.cc:15733` skips vector indexes in the DD index-algorithm validation loop
- `dict_stats_should_ignore_index()` skips them
- Index parameters persist in `dd::Index::options()` (`PERCONA_VECTOR_INDEX_TYPE_KEY`,
  `PERCONA_VECTOR_CONSTRUCTION_PARAMS`) and are read back into `KEY` by
  `fill_vector_construction_params_from_dd()`
- `HA_VECTOR` gates through `sql_table.cc` and `sql_show.cc`

### `innodb_hnsw_max_memory` — why it does not port

The old implementation refused at three hnswlib-specific points: `vec_load` (initial capacity),
`vec_insert_point` entry, and `resizeIndex` (capacity doubling). Dmitry's class has **no resize
and no capacity** — nodes are arena-allocated one block at a time, and the arena has no
per-block free:

```c
void *raw_mem = allocator.allocate(ALIGN_SIZE(sizeof(Node)) + ...);
// TODO: revisit once we add memory limits.        <- hnsw.h:593 (also :281)
if (raw_mem == nullptr) throw std::bad_alloc();
```

`Vec_arena::allocate()` is the natural chokepoint for a server-wide budget — all node memory
flows through it, so one atomic counter covers every table and every index, which is the scope
the sysvar promises. But refusing *inside* `allocate()` is wrong: `nullptr` becomes a throw
**mid-insert**, after neighbours may already be rewired and after the persistor may have written
aux rows on the sub-transaction, with no per-block free to unwind to.

So the enforcement moves to a **pre-flight check at insert entry**: bound the worst-case bytes a
single insert can charge — `sizeof(Node) + vec_size + (max_layer + 2) * M * sizeof(Node *)` —
and refuse before calling `insert()`. The user-visible behaviour is unchanged from the old
branch: the INSERT is rejected with `DB_OUT_OF_MEMORY` → `ER_OUT_OF_RESOURCES`, and the sysvar
stays dynamic so a blocked workload can be unblocked without a restart.

Open with Dmitry: is `insert()` exception-safe mid-mutation? And `max_elements` is currently
both unread and unsettable — the parser recognises only `M` and `metric` — so its relationship
to this budget is undefined.

---

## Phases

### P0 — seam fix

`parse_options()` takes `Key_spec` (DDL-time). At table open we have `KEY`. Same two fields,
different structs, so refactor to the fields and keep two thin overloads:

```c
bool parse_options(LEX_CSTRING type, const Construction_params *params, VectorIndexParam &vip);
bool parse_options(const Key_spec &, VectorIndexParam &);   // DDL
bool parse_options(const KEY &,      VectorIndexParam &);   // open
```

Worth offering upstream rather than carrying. While there: `ef_construction` is a field the
parser never assigns, so the graph is not tunable at all until it is wired.

### P1 — aux DDL

Ports from the old series, in its commit order, with the audit applied:
module skeleton → CREATE → RENAME → TRUNCATE → ALTER ADD/DROP → DISCARD/IMPORT →
counter persistence → `percona_vec_aux_id` populate and rebuild stamp → INSTANT/BULK blocks →
COPY-only ADD → CASCADE-FK fence-post.

No HNSW dependency; lands and tests standalone.

### P2 — runtime and write path

The persistence hooks are carried on this branch as a cherry-pick of
`6bec2b8de86` (Dmitry's `vector-mvp-11267`, PR percona/percona-server#6131), which sits on the
same phase-1 commit our base builds on and therefore applies cleanly. **Drop that commit once
PS-11267 lands in `vector-mvp`.**

An earlier revision of this plan said the persistor was "not reachable from this repository at
all". That was wrong: `git log --all -S` searches only what has been fetched, and the branch had
not been. The commit was one `git fetch` away.

With it, the class is what the design assumes:

```c
template <typename ArenaAllocator, typename Persistor>
class HNSW {
```

One contract detail the design did not state explicitly, now that the header does:

> Must be **stateless**: no mutable per-call, per-transaction, or per-thread fields. All such
> state belongs in `Context`... Callbacks must not rely on data written to `Persistor` members
> during a prior call.

So `Vec_persistor` carries no members at all — trx, aux `dict_table_t`, THD and the `dberr_t`
error slot all live in `Context`. The design already placed them there, but by inference rather
than because the contract demanded it.

The InnoDB-side work: `dict_index_t::vec` as a raw pointer (the struct is never constructed or
destructed — zeroed memory plus `dict_mem_fill_index_struct()` — so it is released by hand in
`dict_mem_index_free()`, as `destroy_fields_array()` already is), then `Vec_arena`,
`Vec_persistor` with its four member-template callbacks, the four `vec_aux_*` functions, the
sub-transaction, and the INSERT / UPDATE / DELETE hooks.

### P3 — load path

`vec_aux_load_node`, record 0 as the entry point, `init_from_entry_point`, stub creation.

### P4 — read path and MVCC

**There is no read path today, at any layer.** `DISTANCE()` is a plain scalar function
(`Item_real_func`, sql/item_strfunc.h:1313), the optimizer has no vector-index awareness at all,
and `handler` has no vector-search API — no analog of `ft_init` / `ft_read`. So
`ORDER BY DISTANCE(...) LIMIT k` is a full table scan plus a sort, and the index is never
consulted. P4 therefore splits across two owners.

#### P4a — server layer

1. **Optimizer.** Recognise `ORDER BY DISTANCE(col, <const>, <metric>) LIMIT k` where `col`
   carries a vector index, and choose that index. Rule-based is enough for the MVP; there is no
   cardinality to cost against, since `dict_stats_should_ignore_index()` skips vector indexes.
2. **Handler API.** New virtuals, shaped like the FT family and mapping onto the class's
   streaming search: `vec_search_init(...)` → `nn_search_start()`,
   `vec_search_next(uchar *buf)` → `nn_search_next()` filtered by MVCC,
   `vec_search_end()` → `NNSearchContext::reset()`. Plus a capability flag and `index_flags`
   that does **not** advertise the index for ordinary scans.
3. **Executor.** An access path that drives those and consumes rows already in distance order, so
   the sort is elided rather than run.
4. **`ef_search` has no home.** `ef_construction` is parsed from `WITH(...)` and applies at build
   time; nothing anywhere carries a *search* width. It needs a session sysvar or a query hint.
5. **Fixes `SELECT COUNT(*)` returning 0**, which is the same defect seen from the other side:
   the optimizer picks the vector index for a covering scan and it has no B-tree. Correct
   `index_flags` removes it.

#### P4b — InnoDB / vector

1. **Streaming search, not `k_nn_search`.** `nn_search_start()` / `nn_search_next()` over an
   `NNSearchContext`, which yields one `base_pk` at a time and carries the visited and discarded
   sets. `vec_knn_search()` uses `k_nn_search` and is a debug probe, not this.
2. **MVCC filter.** Check ② (view-gated visibility) works today with `base_pk` alone, via
   `row_search_on_row_ref()` → `lock_clust_rec_cons_read_sees()` →
   `row_vers_build_for_consistent_read()`.
3. **Pull another candidate for each one filtered out.** This is what the streaming API buys:
   replacing a filtered row costs one more `nn_search_next()`, not a wider search re-run from the
   entry point, and a node is never returned twice. End-of-stream means the graph is exhausted.
4. `ha_innobase::vec_search_*` implementing whatever P4a settles on.
5. An interpreter probe, so P4b is testable before P4a exists.

#### The contract between them

This is the part to agree before either side is written.

- **In:** index number; query vector as raw bytes in the stored column's encoding
  (`dims * sizeof(float)`, little-endian floats); `k`; `ef_search`.
- **Out:** rows in ascending graph distance, already MVCC-filtered.
- **The engine may return fewer than `k` rows even when more matching rows exist.** This is not
  an error and not end-of-data-as-exhaustion — it is what an approximate index does, made worse
  by MVCC filtering removing candidates after the graph chose them. The optimizer must not
  assume the `LIMIT` is satisfied, and must not "fix up" a short result by falling back to a
  scan unless it means to change the query's semantics.
- No duplicate rows within one search.
- Distance is *graph* distance, not a re-evaluated `DISTANCE()`. If the plan also projects
  `DISTANCE(...)`, the server recomputes it; the two agree in value but only the projection is
  exact.

#### Blocked, and what it costs

Check ① (`row.percona_vec_aux_id == node_id`) needs a node id that `k_nn_search` does not return
— it builds its result from `node->base_pk()` alone (vector-common/hnsw.h:324). The consequence
must be stated plainly rather than filed as a nice-to-have: **without ①, a vector UPDATE silently
mis-ranks results**, because the superseded node still names a live primary key and check ②
admits it. That gates GA even though it does not gate the code landing.

---

## Found during P1, feeds into P2

**NULL vectors no longer exist.** Upstream requires `VECTOR NOT NULL` on an indexed column, so
the "row exists but has no vector" state is unreachable. The design's fresh-label rule for
UPDATE-to-non-NULL exists to handle a vector arriving where there was none; that case is now
impossible and the rule may be dead. Confirm before implementing UPDATE.

**The label counter resets on restart.** `vec_aux_autoinc_next_id` is a plain atomic zeroed by
`mem_heap_zalloc`, so ids are reissued after a restart. Benign only while the aux table is
empty, which stops being true the moment P2 writes a row. Persisting the counter is therefore a
prerequisite of the write path, not a follow-up to it.

## Blocked on upstream

1. `k_nn_search` must return the node id — blocks check ①, and with it correctness under UPDATE
2. `load_node_cb` needs a failure path
3. A neighbour list handed to `update_neighbors_cb` must be captured atomically with the
   mutation it describes
4. Memory limits need per-block arena free and an unload transition
5. Is `insert()` exception-safe mid-mutation? Is `max_elements` a cap or a sizing hint?
