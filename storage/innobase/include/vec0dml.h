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
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db0err.h"
#include "dict0mem.h"

class ReadView;
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

/** INSERT one label into the _dead table — the delete mechanism. No
explicit deleter column: the row's hidden DB_TRX_ID is the deleter's
identity, so a reader's own read view decides whether the delete is
visible to it, and rollback is just undo removing the row.
@return DB_SUCCESS, or DB_DUPLICATE_KEY if the label is already dead */
dberr_t vec_aux_dead_insert(trx_t *trx, dict_table_t *dead, uint64_t label);

/** Collect the dead-label set visible under `view` (nullptr = latest)
from a _dead table. See the definition for the visibility contract.
@return DB_SUCCESS or error */
dberr_t vec_aux_load_dead_set(dict_table_t *dead, ReadView *view,
                              std::unordered_set<uint64_t> *labels);

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

/** DELETE superseded edge-list version rows. See the definition in
vec0dml.cc for the purgability rule and why rows carrying a row_ref are
excluded from `losers` by the caller. */
dberr_t vec_aux_collapse_versions(
    trx_t *trx, dict_table_t *aux,
    const std::vector<std::pair<uint64_t, uint32_t>> &losers);

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
