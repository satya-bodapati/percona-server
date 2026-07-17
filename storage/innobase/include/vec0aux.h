/*****************************************************************************

Copyright (c) 2026, Percona Inc.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/vec0aux.h
Auxiliary tables for vector (HNSW) indexes.

One aux table per vector index, named "<db>/vec_<table_id>_<index_id>".
All DDL goes through the InnoDB C API (dict_mem_*, row_create_*_for_mysql,
row_drop_table_for_mysql, row_rename_table_for_mysql) — never through
pars_sql/que_eval_sql, which serializes on the global pars_mutex. */

#ifndef vec0aux_h
#define vec0aux_h

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "data0types.h"
#include "dict0mem.h"
#include "sync0rw.h"
#include "trx0trx.h"
#include "univ.i"

class THD;

/* Forward declarations so this engine-wide header never pulls in the
hnswlib headers; only vec0aux.cc sees the library. */
namespace hnswlib {
template <typename dist_t>
class HierarchicalNSW;
class L2Space;
}  // namespace hnswlib

/** Lowercase on-disk / DD prefix shared by all vector aux tables. */
extern const char *VEC_AUX_PREFIX;

/** Hidden auxiliary column added to a base table that owns >= 1 vector
index. Type: BIGINT UNSIGNED NOT NULL; no secondary index. */
#define VEC_IDX_ID_COL_NAME "vec_idx_id"

/** Number of user columns in a vector aux table.

DEVIATION FROM FTS: FTS uses multiple aux table shapes selected by
suffix — 6 per-index shapes (INDEX_1..INDEX_5, DELETED_CACHE) plus 5
per-table common shapes (CONFIG, DELETED, ADDED, BEING_DELETED,
BEING_DELETED_CACHE) — because FTS's inverted-index storage splits
tokens across hash buckets and keeps per-index state separate from
per-table state. Vector HNSW has different semantics: one aux row
per graph vertex, all vertices in one table. A single fixed schema
(id, vec, row_ref, level, neighbors) is sufficient and simpler. If
phase 2 needs additional shape variance (e.g., a separate CONFIG
aux for HNSW parameters), we'd add it symmetrically then. */
constexpr ulint VEC_AUX_TABLE_NUM_COLS = 5;

/** Column lengths in a vector aux table. */
constexpr ulint VEC_AUX_ID_COL_LEN = 8;         /* BIGINT UNSIGNED */
constexpr ulint VEC_AUX_VEC_COL_LEN = 0;        /* BLOB: 0 = variable */
constexpr ulint VEC_AUX_ROW_REF_COL_LEN = 3072; /* VARBINARY(3072) */
constexpr ulint VEC_AUX_LEVEL_COL_LEN = 1;      /* TINYINT */
constexpr ulint VEC_AUX_NEIGHBORS_COL_LEN = 0;  /* BLOB: 0 = variable */

/** Registered index TYPEs — full definition in vec0index.h (opaque
here: vec0index.h includes this header). */
enum class Vec_index_type : uint8_t;

/** Build the on-disk aux table name for one vector index:
"<db>/vec_<type>_<parent_table_id>_<index_id>", e.g.
"test/vec_hnsw_4a_5b" (SPANN R4: the registry's type token makes the
datadir self-describing and gives every TYPE its own namespace —
spann's three tables become vec_spann_<t>_<i>[/_meta/_dead] without
ambiguity).

@param[in]      parent          parent table that owns the vector index
@param[in]      index_id        id of the vector index (from dict_index_t)
@param[in]      type            the index's registered TYPE (names the
                                token embedded in the name)
@param[out]     name_out        destination buffer (>= MAX_FULL_NAME_LEN)
@param[in]      name_out_len    size of destination buffer */
void vec_aux_get_table_name(const dict_table_t *parent, space_index_t index_id,
                            Vec_index_type type, char *name_out,
                            size_t name_out_len);

/** True if `name` starts with the reserved "vec_" prefix (ALL types).
Used to hide aux tables from INFORMATION_SCHEMA / SHOW TABLES and to
reserve the namespace at CREATE. */
bool vec_aux_is_aux_table_name(const char *name);

/** Parse a "<db>/vec_<type>_<parent_id>_<index_id>" name into its
components. The type token must resolve in the registry
(vec_index_by_name) — a vec_-prefixed name that does not parse is a
reserved-but-invalid name, never an aux table. Used at DD reload time
(dd_open_table_one) to reconstruct dict_table_t::parent_id and
DICT_TF2_VEC_AUX from the on-disk name. Any output pointer may be
nullptr. Returns false if `name` does not match the vector aux
pattern. */
bool vec_aux_parse_table_name(const char *name, table_id_t *parent_id_out,
                              space_index_t *index_id_out,
                              Vec_index_type *type_out = nullptr);

/** Create one aux table for a single vector index. Uses the InnoDB C API
only (no pars_sql).
@param[in,out] trx        transaction
@param[in]     parent     parent table — its space, flags and flags2 are
                          inherited so the aux lives in the right place
@param[in]     index_id   id of the vector index this aux belongs to
@return DB_SUCCESS on success */
dberr_t vec_aux_create_one_table(trx_t *trx, const dict_table_t *parent,
                                 space_index_t index_id);

/** Create aux tables for every vector index already attached to `parent`. */
dberr_t vec_aux_create_all_tables(trx_t *trx, const dict_table_t *parent);

/** DD-register every vector aux table already attached to `parent`.
The in-memory dict_table_t entries must have been created by
@ref vec_aux_create_all_tables / @ref vec_aux_create_one_table first.
Mirrors fts_create_index_dd_tables. Returns true on success. */
bool vec_aux_create_dd_tables(dict_table_t *parent);

/** Drop the aux table for a single vector index. */
dberr_t vec_aux_drop_one_table(trx_t *trx, const dict_table_t *parent,
                               space_index_t index_id);

/** Drop every vector aux table belonging to `parent`. */
dberr_t vec_aux_drop_all_tables(trx_t *trx, dict_table_t *parent);

/** Flip every vector aux table belonging to `parent` from pinned
(can_be_evicted=false, the default from row_create_table_for_mysql)
to evictable, so dict_sys can LRU them out later. Mirrors
fts_detach_aux_tables. Called on both success and fail paths of ALTER
prepare — the "make evictable" side of aux lifecycle. Safe on aux
tables that aren't currently cached (skips silently).
@param[in]  parent          parent that owns the vector indexes
@param[in]  dict_locked     true iff caller already holds dict_sys mutex */
void vec_aux_detach_tables(const dict_table_t *parent, bool dict_locked);

/** True iff `table` has at least one vector index attached. */
bool vec_aux_table_has_vector_index(const dict_table_t *table);

/** Rename every vector aux table belonging to `parent` after the parent
itself has been renamed to `new_parent_name`. Mirrors fts_rename_aux_tables.
Only the db-prefix portion of the aux name changes — the suffix is
keyed by (table_id, index_id) which are invariant under RENAME. Caller
must have verified that the schema actually changed (cross-schema
rename); no early-out check here.

@param[in,out] trx                transaction
@param[in]     parent             dict_table_t of the parent (still
                                  registered under its OLD name in dict_sys)
@param[in]     new_parent_name    new full name of the parent ("db/tbl")
@param[in]     replay             whether running inside crash-recovery
                                  replay
@return DB_SUCCESS on success */
dberr_t vec_aux_rename_tables(trx_t *trx, dict_table_t *parent,
                              const char *new_parent_name, bool replay);

/** Add the hidden vec_idx_id column (BIGINT UNSIGNED NOT NULL) to the
in-memory `dict_table_t` and set DICT_TF2_VEC_HAS_IDX_ID. Mirrors
fts_add_doc_id_column for FTS_DOC_ID. Called both at CREATE time and
during DD load when the dd::Table has a hidden vec_idx_id.

@param[in,out]  table   dict_table_t under construction
@param[in,out]  heap    memory heap for column allocation */
void vec_add_idx_id_column(dict_table_t *table, mem_heap_t *heap);

/** Atomically assign the next vec_idx_id for a row about to be inserted.
Valid ids start at 1. Stamped into the hidden vec_idx_id dfield by the
INSERT path. See the implementation comment for the phase-1 persistence
caveat. */
uint64_t vec_assign_next_idx_id(dict_table_t *table);

/** Stamp the hidden vec_idx_id dfield in `row` with the next id from
the per-table counter. No-op for tables without the hidden column.
Allocations come from `heap` so they outlive this call. Called from
the INSERT path (mirrors fts_create_doc_id). */
void vec_stamp_idx_id(dict_table_t *table, dtuple_t *row, mem_heap_t *heap);

/* ------------------------------------------------------------------
In-memory vector runtime state (PS-11300 phase 2a; genericized by
SPANN R2). */

class Vector_index;

/** What EVERY vector-index runtime shares: the SQL-facing identity of
the index it serves, plus the type implementation that owns it. One
instance hangs off dict_table_t::vec while a runtime is open; ONLY the
implementation that allocated it may interpret the subtype (vec_t for
TYPE hnsw). Everything type-specific — structure pointers, locking
discipline, state machine, side maps, memory accounting — lives in the
subtype, invisible to the rest of the engine. */
struct Vec_runtime {
  /** back pointer */
  dict_table_t *table{nullptr};
  /** the type implementation that allocated this runtime; set before
  publication under dict_sys mutex, so teardown can always dispatch
  table->vec->impl->close() without resolving the TYPE again */
  const Vector_index *impl{nullptr};
  /** the (single, PS-11264) vector index this runtime serves */
  space_index_t index_id{0};
  /** MySQL field ordinal of the vector column (write_row extraction) */
  uint16_t field_no{0};
  /** vector dimensions (floats) */
  uint32_t dims{0};

  virtual ~Vec_runtime() = default;
};

/** The TYPE hnsw runtime — the analog of fts_t.

DEVIATION FROM FTS: fts_t carries background-thread state, a token
cache, an indexes vector and its own allocation heap; vec_t is only the
graph handle plus parameters, created LAZILY at first table open (FTS
creates fts_t eagerly in dict_mem_table_create). Promote fields to
Vec_runtime — not a new global — only if they are provably
type-independent. */
struct vec_t : public Vec_runtime {
  /** HNSW construction parameters, from the WITH() options */
  int M;
  int ef_construction;
  /** graph built from the aux table */
  bool loaded;
  /** exception fallback only: an addPoint threw mid-flight and the
  graph may hold a half-linked node — rebuilt from the aux on next
  use. Rollback of clean inserts does NOT use this (C6 delete-marks
  instead). */
  std::atomic<bool> stale;
  /** S: addPoint + its callback persistence (concurrent inserts run
  in parallel — hnswlib locks internally); X: resizeIndex and the
  exception reload. */
  rw_lock_t latch;
  hnswlib::L2Space *space;
  hnswlib::HierarchicalNSW<float> *hnsw;
  /** bytes charged against the global budget (consumed by C5) */
  std::atomic<uint64_t> mem_used;

  /** label -> serialized base-PK image (row_ref), the kNN path's link
  from a graph hit back to the base row. Rebuilt at vec_load from the
  aux, maintained by insert / PK-refresh (with REFRESHED rollback
  tracking). Entries for tombstoned or rolled-back labels are left in
  place: such nodes are never returned by search, and a DELETE
  rollback revalidates the entry. Guarded by row_ref_mutex (NOT the
  rw-latch: concurrent inserters both hold the latch in S). */
  std::mutex row_ref_mutex;
  std::unordered_map<uint64_t, std::string> row_ref_map;
};

/** Get-or-create table->vec (lazy; thread-safe via dict_sys mutex).
Parameters are only applied on creation. `impl` is the Vector_index
that owns the runtime (stored in Vec_runtime::impl before
publication). */
vec_t *vec_open(dict_table_t *table, const Vector_index *impl,
                uint16_t field_no, uint32_t dims, int M, int ef_construction);

/** Build (or rebuild, if stale) the in-memory graph from the aux table
under the X latch, install the persistence callbacks, and restore
table->vec_next_id from the aux's max id. No-op when already loaded and
not stale. Called at table open — the "first access" load — and as the
exception fallback. */
dberr_t vec_load(dict_table_t *table, THD *thd);

/** Insert one point: resize-if-needed, addPoint under the S latch,
persistence via the callbacks on `trx` (the user transaction). vec_data
must hold vec->dims floats; row_ref is the serialized base PK. */
dberr_t vec_insert_point(trx_t *trx, dict_table_t *table, THD *thd, uint64_t id,
                         const float *vec_data, const byte *row_ref,
                         ulint row_ref_len);

/** DELETE (or the delete half of a vector-column UPDATE) of one graph
point: tombstone the aux row (row_ref -> NULL, geometry kept) on the
user trx, markDelete the in-memory node, and record a MARKED tracking
entry so rollback unmarks it (the undo log un-tombstones the aux row).
@return DB_SUCCESS or error */
dberr_t vec_delete_point(trx_t *trx, dict_table_t *table, THD *thd,
                         uint64_t label);

/** The base row's PRIMARY KEY changed but its vector did not: repoint
the aux row's row_ref at the new PK image. Pure aux-row update — no
graph change, no tracking (the undo log alone inverts it).
@return DB_SUCCESS or error */
dberr_t vec_refresh_row_ref(trx_t *trx, dict_table_t *table, THD *thd,
                            uint64_t label, const byte *row_ref,
                            ulint row_ref_len);

/** Read the hidden vec_idx_id column out of a clustered-index record
(the fts_get_doc_id_from_rec analog; row0sel captures it at fetch time
into row_prebuilt_t::vec_idx_id for the DELETE/UPDATE hooks). */
uint64_t vec_get_idx_id_from_rec(const dict_table_t *table, const rec_t *rec,
                                 const dict_index_t *index);

/** Free the graph, latch and vec_t itself; charge released. Safe on
tables that never opened a graph. */
void vec_close(dict_table_t *table);

/** Total bytes currently charged by all in-memory vector graphs. */
uint64_t vec_total_memory();

/** @return the graph's element count (0 if no graph is loaded) —
lets hnswlib-agnostic callers (ha_innodb) size their resume loop. */
size_t vec_graph_size(const dict_table_t *table);

/* ------------------------------------------------------------------
Per-transaction graph-mutation tracking (rollback support).

The aux rows a transaction writes are protected by the undo log like
any row. The in-memory graph is not: without tracking, a rolled-back
INSERT leaves a live node whose aux row is gone. Each graph mutation
is therefore recorded on the transaction and inverted when (and only
as far as) the transaction rolls back. */

enum class vec_trx_op_type : uint8_t {
  /** addPoint ran for this label; rollback marks it deleted */
  ADDED,
  /** markDelete ran for this label (DELETE); rollback unmarks it */
  MARKED,
  /** the label's row_ref map entry was repointed (PK-only UPDATE);
  rollback restores the old image (the aux row itself is restored by
  the undo log) */
  REFRESHED,
};

struct vec_trx_op_t {
  dict_table_t *table; /*!< base table (pinned by the trx's locks) */
  uint64_t label;      /*!< vec_idx_id of the affected node */
  vec_trx_op_type type;
  undo_no_t undo_no; /*!< trx->undo_no BEFORE the mutation's aux DML;
                     the op is inverted iff the rollback target is at
                     or below this number */
  /** REFRESHED only: previous row_ref image to restore (the base PK
  is a single BIGINT UNSIGNED, PS-11264 — 8-byte storage image) */
  byte old_ref[8]{};
  ulint old_ref_len{0};
};

struct vec_trx_ops_t {
  std::vector<vec_trx_op_t> ops; /*!< in undo_no order (appended) */
};

/** Record one graph mutation on the transaction (lazily allocates
trx->vec_ops). old_ref/old_ref_len are REFRESHED-only. */
void vec_trx_record(trx_t *trx, dict_table_t *table, uint64_t label,
                    vec_trx_op_type type, undo_no_t undo_no,
                    const byte *old_ref = nullptr, ulint old_ref_len = 0);

/** One kNN hit: closer-first order. */
struct vec_knn_hit_t {
  uint64_t label;
  float dist;
  std::string row_ref; /*!< empty when the label has no map entry */
};

/** Approximate kNN over the in-memory graph (loads it on demand).
Search width is max(k, ef) — hnswlib's ef floor — so `ef` is the
per-query recall knob without touching the shared graph state.
markDeleted nodes (tombstones, rolled-back inserts) are excluded by
hnswlib itself. Results are graph candidates: callers that return
rows must fetch each row_ref under their own read view and skip
misses (uncommitted/rolled-back points).
`exclude` (may be nullptr) drops those labels from the result — the
resumable-search hook: the handler re-searches with a grown k,
excluding what it already returned.
@return DB_SUCCESS or error (load failure, dims mismatch) */
dberr_t vec_knn_search(dict_table_t *table, THD *thd, const float *query,
                       uint32_t dims, size_t k, size_t ef,
                       std::vector<vec_knn_hit_t> *hits,
                       const std::unordered_set<uint64_t> *exclude = nullptr);

/** Invert the graph mutations undone by a rollback. Called from
trx_rollback_to_savepoint_low after the undo pass; savept == nullptr
means full rollback (invert everything and free the list). Inversion
is in REVERSE order of recording. A label the graph no longer knows
(a stale-exception reload happened in between) is silently skipped —
the reload already excluded uncommitted rows. */
void vec_trx_rollback(trx_t *trx, const trx_savept_t *savept);

/** Free trx->vec_ops without inverting (commit path). */
void vec_trx_free(trx_t *trx);

/** innodb_hnsw_max_memory: byte budget for ALL in-memory HNSW graphs.
Graph loads and capacity growth that would cross it are refused — the
INSERT (or the deferred load) fails with DB_OUT_OF_MEMORY until the
limit is raised. DEVIATION FROM FTS: innodb_ft_total_cache_size is
READONLY and triggers sync-to-disk when crossed; graphs cannot spill,
so ours refuses growth instead and is dynamic (SET GLOBAL) so a stuck
workload can be unblocked without restart. */
extern unsigned long long vec_hnsw_max_memory;

#endif /* vec0aux_h */
