# Vector Search (kNN read path) — Design

Phase 2b of the vector-index work (Jira id TBD; continues `ps-11300-hnsw-populate`).
Sources evaluated: `martin/vector-cost-based` = `martin/vector` (b72aad1fc53,
"Make the optimizer pick up vector index access from the WHERE clause") + one
`wip` commit (004d1ea5c2d, cost-based integration), and Catalin's PR #5976
(PS-11167, `DISTANCE()`/`VECTOR_DISTANCE()` functions).

## 1. Locked decisions

1. **Distance functions come from Catalin's PR #5976**, cherry-picked onto our
   branch — not Martin's `Item_func_vec_distance`. Catalin's
   `Item_func_vector_distance` is production-grade (5 metrics, SIMD dispatch
   incl. AVX-512/SVE2, double-precision accumulation, 600+ lines of gunit) and
   structurally identical for optimizer purposes (3-arg, `functype()` enum).
   Martin's item was scaffolding; only his *recognition/execution* layers are
   ported.
2. **Index-served execution is restricted to the canonical kNN shape**:
   single-table query block where the distance call references the indexed
   vector column against a constant query vector AND is the single
   `ORDER BY` expression with a `LIMIT`. Everything else (distance in WHERE,
   in projection, no LIMIT, joins) executes exactly — table scan + Catalin's
   function. **DEVIATION FROM MARTIN'S BRANCH**, which activates the index for
   a distance call anywhere in the query: ANN inside a WHERE predicate
   (`WHERE distance(...) <= r`) silently returns *approximately* the right
   rows — searchKnn can miss true neighbors, so qualifying rows disappear
   versus an exact scan. `ORDER BY ... LIMIT k` is the industry-standard
   "approximate kNN" contract users opt into; a filter is not.
3. **Heuristic activation first, cost-based later.** Martin's `wip` commit
   moves activation into the standard `Key_use`/`find_best_ref` machinery
   (`VEC_KEYPART`, `idx_type VECTOR`) with a `k·log2(N)` cost model. The
   architecture is right (mirrors FT), but it is unfinished: hand-waved cost
   constants, a commented-out eqref heuristic, and no re-recorded test
   results. Since decision 2 removes the WHERE-shape competition that
   cost-based choice mainly arbitrates, we start with the deterministic
   heuristic activation (deviating from the `wip`) and keep the `Key_use`
   integration as a follow-up when there are real statistics to feed it.
4. **Old optimizer only** (both Martin commits assert
   `!using_hypergraph_optimizer`). Under hypergraph, vector queries plan as
   ordinary scans (correct, just not accelerated). Documented limitation.
5. **The vec aux table stays invisible to query execution.** Search touches
   only the in-memory graph plus normal base-row fetches. The prototype's
   handler API is base-table-scoped and is kept; nothing new opens the aux at
   query time.
6. Executed on the current branch; Catalin's commit is cherry-picked (single
   squashed commit `09784161dfe`, near-identical 9.7 merge-base — expected
   clean).

## 2. SQL surface (from #5976, unchanged)

`DISTANCE(expr, expr, 'METRIC')` and synonym `VECTOR_DISTANCE(...)`;
metrics EUCLIDEAN | EUCLIDEAN_SQUARED | COSINE | DOT_PRODUCT | MANHATTAN.
`Item_func_vector_distance : Item_real_func`, `functype() ==
VECTOR_DISTANCE_FUNC`. Exact SIMD evaluation lives in
`vector-common/vector_distance.*` and always works, index or not.

## 3. Recognition (port of Martin's plumbing, adapted)

- **Registration**: `Item_func_vector_distance::do_itemize` registers the item
  in `query_block->vector_func_list` when parsed in `CTX_ORDER_BY`
  (Martin also registered `CTX_WHERE`; we register it too so the optimizer
  can *see* the shape, but only the ORDER BY shape activates the index).
  `Query_block` gains `vector_func_list` + the derived-table/subquery merge
  hooks (sql_lex.h/.cc, sql_resolver.cc — port as-is).
- **Activation** (`JOIN::optimize_vector_query`, heuristic form): runs after
  join-order selection; requires
  - single ORDER BY element whose expression is a registered distance call,
  - one argument is `Item_field` on a `MYSQL_TYPE_VECTOR` column, the other
    `const_for_execution()`,
  - the table has a usable `HA_VECTOR` key on exactly that column
    (`keys_in_use_for_query` respects `IGNORE INDEX`/`NO_INDEX` hints),
  - **metric servable by the index** (§6),
  - a finite LIMIT (`m_select_limit != HA_POS_ERROR`),
  - single-table query block (joins: later, with the cost-based port).
  On match: `tab->set_type(JT_VECTOR)`, `set_index`, `set_vec(const_expr)`,
  `set_vec_limit(limit)`, and the sort is elided
  (`m_ordered_index_usage = ORDERED_INDEX_ORDER_BY` path, as in Martin's
  test_if_skip_sort_order hunk gated on `JT_VECTOR`). Join buffering is
  disallowed for `JT_VECTOR` (wip's assert, kept).
- **COUNT(*) fix rides along**: the phase-1 bug (optimizer scans the vector
  stub index for COUNT(*)) is fixed in the same area — the vector index is
  excluded from `find_shortest_key`-style selection; it is never a scannable
  index.

## 4. Execution

`VectorSearchIterator` (ported from the prototype: DoInit → `vec_init`,
DoRead → `vec_read_first(m_item, buf, m_limit)` then `vec_read_next(buf)`),
instantiated for `JT_VECTOR`. Handler API (base table):

```
virtual int vec_init();
virtual int vec_read_first(Item *query_vec, uchar *buf, ha_rows limit);
virtual int vec_read_next(uchar *buf);
```

`ha_innobase` implementation — replaces the prototype's body entirely:

1. Evaluate the query-vector Item once; validate dims == `vec->dims`.
2. Under `vec->latch` (S): `searchKnnCloserFirst(query, k)` where
   `k = min(limit + overfetch_headroom, cur_element_count)`. markDeleted
   nodes (our DELETE tombstones and rolled-back points) are excluded by
   hnswlib itself.
3. For each candidate label: resolve `row_ref` via the in-memory map (§5),
   build the PK search tuple, `row_search_for_mysql(ROW_SEL_EXACT)` under the
   session's normal read view and lock mode.
   - Row found → return it.
   - Row not visible / not found (uncommitted point from another trx,
     rolled-back point not yet inverted, base row deleted after the graph
     snapshot) → **skip and continue**. FIXES the prototype bug of returning
     `HA_ERR_END_OF_FILE` on the first miss, which under concurrency
     truncates results.
4. Candidates exhausted before `limit` rows returned → **resume**: re-search
   with grown k (doubling), excluding already-returned labels via the fork's
   `BaseFilterFunctor`. Terminates when the graph is exhausted
   (`k >= cur_element_count` and no new candidates). This replaces the
   prototype's self-described "horrible" resume with the same primitive but
   bounded growth; still O(returned) filter state — acceptable, k is
   LIMIT-sized.
5. MVCC statement: search results are a snapshot of the graph at step 2;
   rows are validated per-fetch under the read view. No graph MVCC —
   uncommitted points cost only wasted candidates (design §11 of the write
   doc, unchanged).

`ef_search`: new session-scoped sysvar `innodb_hnsw_ef_search`
(default 40, min 1) applied via `hnsw->setEf()` before the search — kNN
recall/latency knob, mirrors hnswlib convention.

## 5. Label → row_ref map

Search returns labels (`vec_idx_id`); fetching rows needs the base PK. The
prototype had label == PK; ours does not (fresh-label rule, tombstones).
`vec_t` gains an in-memory map `label -> row_ref` (8-byte PK image today):

- built during `vec_load` from the loader's rows (loader extended to emit
  `row_ref` images alongside the tuples),
- inserted by the addPoint insert-callback path (`vec_insert_point` has the
  serialized row_ref in hand),
- updated by `vec_refresh_row_ref` (PK-only UPDATE),
- entries for tombstoned labels are left in place (markDeleted nodes are
  never returned by search; on rollback of the DELETE the entry is valid
  again — symmetric with unmarkDelete).

Guarded by the existing `vec->latch` (S for read/insert beside addPoint, X
during load) — no new synchronization. Memory: 16B/node + hash overhead,
charged to the existing accounting.

## 6. Metric matching

The index's construction metric comes from `WITH (metric=...)`
(default euclidean; hnswlib `L2Space` computes *squared* L2).
Servability rule for `ORDER BY DISTANCE(v, q, M) LIMIT k`:

| query metric        | L2 graph serves? | why                                   |
|---------------------|------------------|---------------------------------------|
| EUCLIDEAN           | yes              | sqrt is monotonic — same order        |
| EUCLIDEAN_SQUARED   | yes              | native                                 |
| COSINE / DOT / MANHATTAN | no          | different geometry — wrong neighbors   |

Mismatch → no activation, exact scan. COSINE/DOT graphs
(`InnerProductSpace`) are future work on the WITH() option.

## 7. Commit map

| # | Commit | Contents | Test |
|---|--------|----------|------|
| S1 | cherry-pick #5976 | DISTANCE()/VECTOR_DISTANCE() + vector-common SIMD core | Catalin's distance_* MTR + gunit (as merged) |
| S2 | InnoDB kNN read primitive | label→row_ref map in vec_t; `innodb_hnsw_ef_search`; internal knn entry (search + visibility loop); debug interpreter `vec_knn <tbl> <csv-vec> <k>` | `vector_knn_debug.test`: storage-level kNN incl. tombstone exclusion, uncommitted-row skip, post-restart parity |
| S3 | handler API + iterator + optimizer | `vec_init/vec_read_first/vec_read_next`, `JT_VECTOR`, `VectorSearchIterator`, registration + `optimize_vector_query` (gated per §3), sort elision, EXPLAIN, COUNT(*) fix | `vector_search.test` (our syntax): kNN correctness vs exact scan, LIMIT/hint/metric gating, EXPLAIN plans, concurrency skip case |
| S4 | (follow-up, not this phase) | cost-based Key_use port, joins, hypergraph, COSINE graphs | — |

## 8. Limitations (documented, deliberate)

- Old optimizer only; hypergraph plans an exact scan.
- Index accelerates only `ORDER BY DISTANCE(...) LIMIT k` on a single table.
- Approximate-kNN semantics: results are hnswlib-approximate; recall tunable
  via `ef_search`/`ef_construction`/`M`.
- `WHERE distance(...) <= r` is always exact (never the index) — by design.
- Query vector must be constant for the statement (no correlated vectors).
