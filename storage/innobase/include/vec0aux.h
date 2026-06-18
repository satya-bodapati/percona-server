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

#include "dict0mem.h"
#include "trx0trx.h"
#include "univ.i"

/** Lowercase on-disk / DD prefix shared by all vector aux tables. */
extern const char *VEC_AUX_PREFIX;

/** Hidden auxiliary column added to a base table that owns >= 1 vector
index. Type: BIGINT UNSIGNED NOT NULL; no secondary index. */
#define VEC_IDX_ID_COL_NAME "vec_idx_id"

/** Number of user columns in a vector aux table. */
constexpr ulint VEC_AUX_TABLE_NUM_COLS = 5;

/** Column lengths in a vector aux table. */
constexpr ulint VEC_AUX_ID_COL_LEN = 8;         /* BIGINT UNSIGNED */
constexpr ulint VEC_AUX_VEC_COL_LEN = 0;        /* BLOB: 0 = variable */
constexpr ulint VEC_AUX_ROW_REF_COL_LEN = 3072; /* VARBINARY(3072) */
constexpr ulint VEC_AUX_LEVEL_COL_LEN = 1;      /* TINYINT */
constexpr ulint VEC_AUX_NEIGHBORS_COL_LEN = 0;  /* BLOB: 0 = variable */

/** Build the on-disk aux table name for one vector index:
"<db>/vec_<parent_table_id>_<index_id>".

@param[in]      parent          parent table that owns the vector index
@param[in]      index_id        id of the vector index (from dict_index_t)
@param[out]     name_out        destination buffer (>= MAX_FULL_NAME_LEN)
@param[in]      name_out_len    size of destination buffer */
void vec_aux_get_table_name(const dict_table_t *parent, space_index_t index_id,
                            char *name_out, size_t name_out_len);

/** True if `name` matches the vec_<id>_<id> aux table pattern. Used to hide
aux tables from INFORMATION_SCHEMA and SHOW TABLES. */
bool vec_aux_is_aux_table_name(const char *name);

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

/** Drop the aux table for a single vector index. */
dberr_t vec_aux_drop_one_table(trx_t *trx, const dict_table_t *parent,
                               space_index_t index_id);

/** Drop every vector aux table belonging to `parent`. */
dberr_t vec_aux_drop_all_tables(trx_t *trx, dict_table_t *parent);

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

#endif /* vec0aux_h */
