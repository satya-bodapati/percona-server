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

/** @file include/vec0dml.h
Row-level DML on vector-index auxiliary tables through the InnoDB query-graph
C API — insert, targeted neighbor update, and MVCC-consistent full load.

DEVIATION FROM FTS: FTS performs its aux-table DML through the internal SQL
parser (fts_parse_sql / fts_eval_sql), which serializes every operation on
the global pars_mutex. Vector aux DML runs on every user INSERT, so it uses
the same parser-free query-graph machinery row0mysql itself uses
(ins_node/upd_node + pars_complete_graph_for_exec) on the user transaction:
full redo/undo/locking, no global mutex. */

#ifndef vec0dml_h
#define vec0dml_h

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db0err.h"
#include "dict0mem.h"
#include "trx0trx.h"
#include "univ.i"

/** One materialized aux row for insertion. Pointers are caller-owned;
vec_aux_insert copies what it needs. */
struct vec_aux_row_t {
  /** vec_idx_id of the base row == HNSW label == aux PK prefix */
  uint64_t id;
  /** edge-list version (H1): 0 = birth row (identity payload present);
  > 0 = a neighbors-only snapshot appended by a rewire. Assigned under
  hnswlib's per-node link lock — version order == mutation order. */
  uint32_t ver{0};
  /** vector data, dims * sizeof(float) bytes */
  const float *vec;
  uint32_t dims;
  /** serialized base PK; nullptr => SQL NULL (tombstone) */
  const byte *row_ref;
  ulint row_ref_len;
  /** HNSW level of this node; must be in [0, 127] */
  int level;
  /** serialized neighbor lists (vec_aux_serialize_neighbors format) */
  const byte *neighbors;
  ulint neighbors_len;
};

/** One row loaded from the aux table:
(id, level, vector, neighbor labels per level). Structurally identical to
hnswlib's VecAuxLoadedRowTuple so the load path converts by construction;
defined here without including hnswlib to keep this header engine-only. */
using vec_loaded_row_t =
    std::tuple<std::uint64_t, std::uint64_t, std::vector<float>,
               std::vector<std::vector<std::size_t>>, std::uint32_t>;

/** Serialize per-level neighbor label lists into the aux `neighbors` BLOB.
Format (big-endian, ported from the PS-10258 prototype):
  [4B nlevels] then per level: [4B count][count x 8B labels]
@param[in]  neighbors_by_level  neighbor labels, one vector per level
@param[out] out                 serialized bytes (replaced) */
void vec_aux_serialize_neighbors(
    const std::vector<std::vector<std::size_t>> &neighbors_by_level,
    std::vector<byte> &out);

/** Inverse of vec_aux_serialize_neighbors.
@return true on malformed input */
bool vec_aux_deserialize_neighbors(
    const byte *data, ulint len,
    std::vector<std::vector<std::size_t>> &neighbors_by_level);

/** Serialize the base row's PRIMARY KEY into the aux `row_ref` column.
The SQL layer restricts vector-indexed tables to a single-column BIGINT
UNSIGNED PK (sql_table.cc prepare_key_column), so this is currently the
8-byte big-endian storage image of that column; defined as "concatenation
of the PK fields' InnoDB storage images" so composite PKs later only relax
the SQL-layer gate.
@param[in]  row    the base table row (ins_node_t::row shape)
@param[in]  table  base table
@param[out] out    destination, at least VEC_AUX_ROW_REF_COL_LEN bytes
@return bytes written, or 0 on error */
ulint vec_row_ref_serialize(const dtuple_t *row, const dict_table_t *table,
                            byte *out);

/** Insert one row into a vector aux table on the given transaction.
Parser-free ins_node execution; on failure the caller's statement fails
and the row is undone with the statement, like any user DML.
@param[in,out] trx  transaction to ride (normally the user trx)
@param[in]     aux  aux table (opened by the caller)
@param[in]     row  row contents
@return DB_SUCCESS or error */
dberr_t vec_aux_insert(trx_t *trx, dict_table_t *aux, const vec_aux_row_t &row);

/** Insert one row into a spann postings table (SPANN S2): PK is
(head_id, label) — closure copies share the label under different
heads. Same parser-free ins_node execution and trx contract as
vec_aux_insert.
@param[in,out] trx          transaction to ride (ALTER trx at build,
                            user trx from S3 on)
@param[in]     aux          postings aux table (opened by the caller)
@param[in]     head_id      owning head's label
@param[in]     label        the row's vec_idx_id
@param[in]     vec          vector data, dims floats
@param[in]     dims         vector dimensions
@param[in]     row_ref      serialized base PK (NOT NULL here — spann
                            deletes go to _dead, never tombstones)
@param[in]     row_ref_len  length of row_ref
@return DB_SUCCESS or error */
dberr_t vec_spann_posting_insert(trx_t *trx, dict_table_t *aux,
                                 uint64_t head_id, uint64_t label,
                                 const float *vec, uint32_t dims,
                                 const byte *row_ref, ulint row_ref_len);

/** Insert one row into a spann _meta table: PK is (mtype, id); mval is
the typed payload (a head's vector bytes for mtype HEAD).
@return DB_SUCCESS or error */
dberr_t vec_spann_meta_insert(trx_t *trx, dict_table_t *aux, uint8_t mtype,
                              uint64_t id, const byte *mval, ulint mval_len);

/** INSERT one label into a _dead table — the shared delete mechanism
for BOTH index TYPEs (spann postings, hnsw edge log). There is no
explicit deleter column: the row's hidden DB_TRX_ID is the deleter's
identity, so a reader's own read view decides whether the delete is
visible to it. Rollback needs nothing special — undo removes the row.
@return DB_SUCCESS, or DB_DUPLICATE_KEY if the label is already dead */
dberr_t vec_aux_dead_insert(trx_t *trx, dict_table_t *aux, uint64_t label);

/** One spann head loaded from _meta: (head label, vector). */
using vec_spann_head_t = std::pair<std::uint64_t, std::vector<float>>;

/** Load every visible HEAD row of a spann _meta table under a fresh
read view (background transaction) — the runtime head-graph source.
A zero-length mval decodes as the zero vector of `dims` (reserved for
bootstrap shapes); any other width mismatch is DB_CORRUPTION.
@param[in]  meta   _meta aux table (opened by the caller)
@param[in]  dims   vector dimensions (from the runtime)
@param[out] heads  visible heads (replaced)
@return DB_SUCCESS or error */
dberr_t vec_spann_meta_load_heads(dict_table_t *meta, uint32_t dims,
                                  std::vector<vec_spann_head_t> *heads);

/** Load every visible row of a vector aux table under a fresh read view
(background transaction), for graph reconstruction.
@param[in]  aux            aux table
@param[in]  dims           expected vector dimensions (row is rejected on
                           mismatch with DB_CORRUPTION); 0 = derive from
                           the first row and enforce consistency
@param[out] rows           visible, non-tombstone rows
@param[out] raw_max_id     max id over ALL records — including delete-marked
                           and versions invisible to the read view — for
                           vec_next_id restoration (committed-id reuse must
                           be impossible; holes are fine)
@param[out] saw_invisible  true if any record/version was skipped for
                           visibility (caller keeps the graph stale-marked
                           and retries later)
@return DB_SUCCESS or error */
dberr_t vec_aux_load_rows(
    dict_table_t *aux, uint32_t dims, std::vector<vec_loaded_row_t> *rows,
    uint64_t *raw_max_id, bool *saw_invisible,
    std::vector<std::pair<uint64_t, std::string>> *row_refs = nullptr,
    std::vector<std::pair<uint64_t, uint32_t>> *collapsible = nullptr,
    uint64_t *n_raw_rows = nullptr);

/** DELETE superseded edge-list version rows (H1-4). See the definition
in vec0dml.cc for the purgability rule and why rows carrying a row_ref
are excluded from `losers` by the caller. */
dberr_t vec_aux_collapse_versions(
    trx_t *trx, dict_table_t *aux,
    const std::vector<std::pair<uint64_t, uint32_t>> &losers);

class ReadView;

/** Per-posting callback of vec_spann_scan_list: (label, vector floats,
row_ref bytes, row_ref length). Pointers are valid only during the
call. */
using vec_spann_posting_fn =
    std::function<void(uint64_t, const float *, const byte *, ulint)>;

/** Range-scan ONE posting list — the clustered-index range with PK
prefix head_id — under the CALLER's read view (SPANN S5): the reader's
MVCC position, not a background snapshot. view == nullptr reads latest
record versions (no version building). Emits only visible,
non-delete-marked postings; dead-label filtering is the caller's job
(vec_aux_load_dead_set under the same view).
@param[in] trx      caller's trx (off-page vector reads)
@param[in] postings postings aux table (opened by the caller)
@param[in] head_id  the list to scan
@param[in] dims     vector dimensions (width mismatch = DB_CORRUPTION)
@param[in] view     caller's read view, or nullptr for latest
@param[in] fn       per-posting callback
@return DB_SUCCESS or error */
dberr_t vec_spann_scan_list(trx_t *trx, dict_table_t *postings,
                            uint64_t head_id, uint32_t dims, ReadView *view,
                            const vec_spann_posting_fn &fn,
                            uint64_t label_gt = 0);

/** DELETE one _meta row by PK (mtype, id) — head retirement (phase
L): a plain row delete on the maintenance transaction, so old readers
keep seeing the head via MVCC and purge reclaims it (never a status
flag). Positioned upd_node with is_delete — locking discipline is
documented on vec_spann_delete_by_pk (vec0dml.cc).
@return DB_SUCCESS, DB_RECORD_NOT_FOUND if no such row, or error */
dberr_t vec_spann_meta_delete(trx_t *trx, dict_table_t *meta, uint8_t mtype,
                              uint64_t id);

/** DELETE one posting row by PK (head_id, label) — the L2 GC shape.
Old readers keep seeing the delete-marked row via MVCC; purge
reclaims it.
@return DB_SUCCESS, DB_RECORD_NOT_FOUND, or error */
dberr_t vec_spann_posting_delete(trx_t *trx, dict_table_t *postings,
                                 uint64_t head_id, uint64_t label);

/** DELETE one _dead row by PK (label) — the tail end of dead-label
GC, once every copy of the label is swept.
@return DB_SUCCESS, DB_RECORD_NOT_FOUND, or error */
dberr_t vec_aux_dead_delete(trx_t *trx, dict_table_t *dead, uint64_t label);

/** Enumerate (head_id, label, row_ref, row_ref_len) of every
non-delete-marked posting record, latest versions — the L2 GC's single
discovery scan. The callback must not run DML (open
mini-transaction); ref points into the page and is valid only during
the call.
@return DB_SUCCESS or error */
dberr_t vec_spann_scan_all_postings(
    dict_table_t *postings,
    const std::function<void(uint64_t, uint64_t, const byte *, ulint)> &fn);

/** Decide whether a posting is a rollback ORPHAN by probing its base
row (L2 GC): the posting is an orphan iff its base PK record is
physically absent in any state (an insert always writes the base row
before its postings, so absence means rollback or full purge), or the
version visible to `view` — pass the OLDEST active view — carries a
DIFFERENT vec_idx_id (PK reuse after the original insert rolled back;
a live posting's label always equals its row's current label).
Conservative on everything young: rows with no version visible to the
oldest view are never called orphans. The caller must have excluded
dead labels first — committed deletes belong to the _dead rule.
@return DB_SUCCESS or error */
dberr_t vec_spann_base_probe(dict_table_t *base, const byte *row_ref,
                             ulint ref_len, ReadView *view, uint64_t label,
                             bool *orphan);

/** Collect the dead-label set visible under `view` (nullptr = latest)
from a _dead table — shared by both index TYPEs. Per-reader delete visibility IS
this scan: each dead row's own hidden DB_TRX_ID decides whether this reader sees
the delete (design §3) — an old view misses young deletes and keeps
returning the row, RYOW sees its own uncommitted delete.
@return DB_SUCCESS or error */
dberr_t vec_aux_load_dead_set(dict_table_t *dead, ReadView *view,
                              std::unordered_set<uint64_t> *labels);

/** Max vec_idx_id over ALL records of the BASE table's clustered index
(visible or not, delete-marked included — conservative, like the aux
loader's raw_max_id). Used to re-seed the counter when its persisted
metadata is unavailable, e.g. after IMPORT TABLESPACE: the imported
rows carry their source-stamped ids in the hidden column, and new
assignments must stay above every one of them (base-id uniqueness,
PS-11300 counter persistence).
@param[in]  base    base table (quiesced: exclusive-MDL context)
@param[out] max_id  max stamped id, 0 for an empty table
@return DB_SUCCESS or error */
dberr_t vec_base_max_idx_id(dict_table_t *base, uint64_t *max_id);

/** One base row collected for the index build:
(stamped vec_idx_id, vector floats, 8-byte base-PK image). */
using vec_base_row_t =
    std::tuple<std::uint64_t, std::vector<float>, std::array<byte, 8>>;

/** Collect every index-relevant base row from the clustered index:
skips delete-marked records and NULL vectors; rejects width-mismatched
vectors with DB_CORRUPTION. Caller context must be quiesced against
writers (ADD INDEX runs with at least a SHARED lock), so the latest
record version is authoritative.
@param[in]  trx        transaction (for off-page vector reads)
@param[in]  base       base table
@param[in]  vec_index  the vector index being built (its field ->
                       vector column)
@param[in]  dims       vector dimensions
@param[out] rows       collected rows
@return DB_SUCCESS or error */
dberr_t vec_base_collect_rows(trx_t *trx, dict_table_t *base,
                              const dict_index_t *vec_index, uint32_t dims,
                              std::vector<vec_base_row_t> *rows);

#endif /* vec0dml_h */
