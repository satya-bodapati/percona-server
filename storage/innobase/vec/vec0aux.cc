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

/** @file vec/vec0aux.cc
Auxiliary tables for vector (HNSW) indexes. Phase 1: creation, drop, rename,
naming. No population — that lands in PS-11300. */

#include "vec0aux.h"

#include <cstdio>
#include <cstring>

#include "data0type.h"
#include "dict0boot.h"
#include "dict0dd.h"
#include "dict0dict.h"
#include "dict0mem.h"
#include "fts0fts.h"
#include "fts0priv.h"
#include "row0mysql.h"
#include "trx0trx.h"
#include "univ.i"
#include "ut0new.h"

const char *VEC_AUX_PREFIX = "vec_";

namespace {

/** Extract the flags2 bits an aux table should inherit from its parent —
file_per_table, encryption, temporary, plus DICT_TF2_AUX. Same set the FTS
aux path preserves (see fts_get_table_flags2_for_aux_tables in fts0fts.cc;
that helper is file-static so we re-derive it here). */
inline uint32_t aux_flags2_from_parent(const dict_table_t *parent) {
  return (parent->flags2 & DICT_TF2_USE_FILE_PER_TABLE) |
         (parent->flags2 & DICT_TF2_ENCRYPTION_FILE_PER_TABLE) |
         (parent->flags2 & DICT_TF2_TEMPORARY) | DICT_TF2_AUX;
}

/** Build the database-prefix portion of a parent name "db/tbl" — returns
the byte length of "db/" (including the slash) or 0 if `parent_name` has no
slash. */
size_t db_prefix_len(const char *parent_name) {
  const char *slash =
      static_cast<const char *>(memchr(parent_name, '/', strlen(parent_name)));
  return slash != nullptr ? static_cast<size_t>(slash - parent_name) + 1 : 0;
}

}  // namespace

void vec_aux_get_table_name(const dict_table_t *parent, space_index_t index_id,
                            char *name_out, size_t name_out_len) {
  ut_a(parent != nullptr);
  ut_a(name_out != nullptr);
  ut_a(name_out_len >= MAX_FULL_NAME_LEN);

  const char *parent_name = parent->name.m_name;
  const size_t db_len = db_prefix_len(parent_name);

  char table_id_str[FTS_AUX_MIN_TABLE_ID_LENGTH];
  int n = fts_write_object_id(parent->id, table_id_str);
  ut_a(n > 0);

  char index_id_str[FTS_AUX_MIN_TABLE_ID_LENGTH];
  n = fts_write_object_id(index_id, index_id_str);
  ut_a(n > 0);

  const int written =
      snprintf(name_out, name_out_len, "%.*s%s%s_%s", static_cast<int>(db_len),
               parent_name, VEC_AUX_PREFIX, table_id_str, index_id_str);
  ut_a(written > 0);
  ut_a(static_cast<size_t>(written) < name_out_len);
}

bool vec_aux_is_aux_table_name(const char *name) {
  if (name == nullptr) return false;
  const char *slash = strchr(name, '/');
  const char *after_db = slash != nullptr ? slash + 1 : name;
  return strncmp(after_db, VEC_AUX_PREFIX, strlen(VEC_AUX_PREFIX)) == 0;
}

bool vec_aux_table_has_vector_index(const dict_table_t *table) {
  if (table == nullptr) return false;
  for (const dict_index_t *idx = UT_LIST_GET_FIRST(table->indexes);
       idx != nullptr; idx = UT_LIST_GET_NEXT(indexes, idx)) {
    if (idx->is_vector()) return true;
  }
  return false;
}

void vec_add_idx_id_column(dict_table_t *table, mem_heap_t *heap) {
  dict_mem_table_add_col(
      table, heap, VEC_IDX_ID_COL_NAME, DATA_INT,
      dtype_form_prtype(DATA_NOT_NULL | DATA_UNSIGNED | DATA_BINARY_TYPE, 0),
      sizeof(uint64_t), false);
  DICT_TF2_FLAG_SET(table, DICT_TF2_VEC_HAS_IDX_ID);
}

namespace {

/** Allocate and fully populate the in-memory dict_table_t for one vector
aux table. Schema is fixed:
  id        BIGINT UNSIGNED NOT NULL PRIMARY KEY,
  vec       BLOB NOT NULL,
  row_ref   VARBINARY(3072),         -- NULLable; no index
  level     TINYINT NOT NULL,
  neighbors BLOB NOT NULL */
dict_table_t *create_in_mem_vec_aux_table(const char *aux_name,
                                          const dict_table_t *parent,
                                          mem_heap_t *heap) {
  dict_table_t *t =
      dict_mem_table_create(aux_name, parent->space, VEC_AUX_TABLE_NUM_COLS, 0,
                            0, parent->flags, aux_flags2_from_parent(parent));

  if (DICT_TF_HAS_SHARED_SPACE(parent->flags)) {
    ut_ad(parent->space == fil_space_get_id_by_name(parent->tablespace()));
    t->tablespace = mem_heap_strdup(t->heap, parent->tablespace);
  }
  if (DICT_TF_HAS_DATA_DIR(parent->flags)) {
    ut_ad(parent->data_dir_path != nullptr);
    t->data_dir_path = mem_heap_strdup(t->heap, parent->data_dir_path);
  }

  /* id BIGINT UNSIGNED NOT NULL */
  dict_mem_table_add_col(t, heap, "id", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED,
                         VEC_AUX_ID_COL_LEN, true);

  /* vec BLOB NOT NULL */
  dict_mem_table_add_col(
      t, heap, "vec", DATA_BLOB,
      (DATA_MTYPE_MAX << 16) | DATA_BINARY_TYPE | DATA_NOT_NULL,
      VEC_AUX_VEC_COL_LEN, true);

  /* row_ref VARBINARY(3072) — nullable, NULL means tombstone */
  dict_mem_table_add_col(t, heap, "row_ref", DATA_BINARY, DATA_BINARY_TYPE,
                         VEC_AUX_ROW_REF_COL_LEN, true);

  /* level TINYINT NOT NULL — stored as 1-byte INT */
  dict_mem_table_add_col(t, heap, "level", DATA_INT, DATA_NOT_NULL,
                         VEC_AUX_LEVEL_COL_LEN, true);

  /* neighbors BLOB NOT NULL */
  dict_mem_table_add_col(
      t, heap, "neighbors", DATA_BLOB,
      (DATA_MTYPE_MAX << 16) | DATA_BINARY_TYPE | DATA_NOT_NULL,
      VEC_AUX_NEIGHBORS_COL_LEN, true);

  return t;
}

}  // namespace

dberr_t vec_aux_create_one_table(trx_t *trx, const dict_table_t *parent,
                                 space_index_t index_id) {
  ut_a(trx != nullptr);
  ut_a(parent != nullptr);

  char aux_name[MAX_FULL_NAME_LEN];
  vec_aux_get_table_name(parent, index_id, aux_name, sizeof(aux_name));

  mem_heap_t *heap = mem_heap_create(1024, UT_LOCATION_HERE);
  dict_table_t *aux = create_in_mem_vec_aux_table(aux_name, parent, heap);

  dberr_t err = row_create_table_for_mysql(aux, nullptr, nullptr, trx, nullptr);

  if (err == DB_SUCCESS) {
    dict_index_t *cidx =
        dict_mem_index_create(aux_name, "VEC_AUX_TABLE_PK", aux->space,
                              DICT_UNIQUE | DICT_CLUSTERED, 1);
    cidx->add_field("id", 0, true);

    const trx_dict_op_t saved_op = trx_get_dict_operation(trx);
    err = row_create_index_for_mysql(cidx, trx, nullptr, nullptr);
    trx->dict_operation = saved_op;
  }

  mem_heap_free(heap);

  if (err != DB_SUCCESS) {
    trx->error_state = err;
    ib::warn(ER_IB_MSG_465) << "Failed to create vector aux table " << aux_name;
  }
  return err;
}

dberr_t vec_aux_create_all_tables(trx_t *trx, const dict_table_t *parent) {
  ut_a(trx != nullptr);
  ut_a(parent != nullptr);

  for (const dict_index_t *idx = UT_LIST_GET_FIRST(parent->indexes);
       idx != nullptr; idx = UT_LIST_GET_NEXT(indexes, idx)) {
    if (!idx->is_vector()) continue;
    dberr_t err = vec_aux_create_one_table(trx, parent, idx->id);
    if (err != DB_SUCCESS) return err;
  }
  return DB_SUCCESS;
}

dberr_t vec_aux_drop_one_table(trx_t *trx, const dict_table_t *parent,
                               space_index_t index_id) {
  ut_a(trx != nullptr);
  ut_a(parent != nullptr);

  char aux_name[MAX_FULL_NAME_LEN];
  vec_aux_get_table_name(parent, index_id, aux_name, sizeof(aux_name));

  const bool file_per_table = dict_table_is_file_per_table(parent);

  dberr_t err = row_drop_table_for_mysql(aux_name, trx, false, nullptr);
  if (err != DB_SUCCESS && err != DB_TABLE_NOT_FOUND) {
    ib::warn(ER_IB_MSG_466) << "Failed to drop vector aux table " << aux_name
                            << " err=" << static_cast<int>(err);
    return err;
  }

  /* row_drop_table_for_mysql only tears down dict_sys + the .ibd. The
  matching dd::Table + dd::Tablespace entries created by
  dd_create_vec_aux_table linger until we explicitly drop them. The FTS
  pattern is identical (see fts_drop_dd_tables); reuse dd_drop_fts_table
  here, which is generic across aux-table kinds. dict_sys mutex must be
  released around the DD client call. */
  const bool dict_locked = trx->dict_operation_lock_mode == RW_X_LATCH;
  if (dict_locked) {
    dict_sys_mutex_exit();
  }
  (void)dd_drop_fts_table(aux_name, file_per_table);
  if (dict_locked) {
    dict_sys_mutex_enter();
  }

  /* Treat NOT_FOUND from the in-memory drop as success — covers tables
  created before this code landed. */
  return err == DB_TABLE_NOT_FOUND ? DB_SUCCESS : err;
}

dberr_t vec_aux_drop_all_tables(trx_t *trx, dict_table_t *parent) {
  ut_a(trx != nullptr);
  ut_a(parent != nullptr);

  for (const dict_index_t *idx = UT_LIST_GET_FIRST(parent->indexes);
       idx != nullptr; idx = UT_LIST_GET_NEXT(indexes, idx)) {
    if (!idx->is_vector()) continue;
    dberr_t err = vec_aux_drop_one_table(trx, parent, idx->id);
    if (err != DB_SUCCESS) return err;
  }
  return DB_SUCCESS;
}

namespace {

/** Build the post-rename aux name. Given the OLD aux name
"old_db/vec_<tid>_<iid>" and the parent's NEW name "new_db/<tbl>", write
"new_db/vec_<tid>_<iid>" into `out`. Mirrors what fts_rename_one_aux_table
does inline. */
void rebuild_aux_name_with_new_db(const char *old_aux_name,
                                  const char *new_parent_name, char *out,
                                  size_t out_len) {
  const ulint new_db_len = dict_get_db_name_len(new_parent_name);
  const ulint old_db_len = dict_get_db_name_len(old_aux_name);
  /* +1 for the slash; +1 for NUL */
  const size_t needed = strlen(old_aux_name) + new_db_len - old_db_len + 1;
  ut_a(needed <= out_len);

  memcpy(out, new_parent_name, new_db_len);
  const char *old_slash = strchr(old_aux_name, '/');
  ut_a(old_slash != nullptr);
  const size_t suffix_len = strlen(old_slash); /* includes leading '/' */
  memcpy(out + new_db_len, old_slash, suffix_len + 1 /* NUL */);
}

}  // namespace

dberr_t vec_aux_rename_tables(trx_t *trx, dict_table_t *parent,
                              const char *new_parent_name, bool replay) {
  ut_a(trx != nullptr);
  ut_a(parent != nullptr);
  ut_a(new_parent_name != nullptr);

  for (const dict_index_t *idx = UT_LIST_GET_FIRST(parent->indexes);
       idx != nullptr; idx = UT_LIST_GET_NEXT(indexes, idx)) {
    if (!idx->is_vector()) continue;

    char old_aux_name[MAX_FULL_NAME_LEN];
    vec_aux_get_table_name(parent, idx->id, old_aux_name, sizeof(old_aux_name));

    char new_aux_name[MAX_FULL_NAME_LEN];
    rebuild_aux_name_with_new_db(old_aux_name, new_parent_name, new_aux_name,
                                 sizeof(new_aux_name));

    dberr_t err = row_rename_table_for_mysql(old_aux_name, new_aux_name,
                                             nullptr, trx, replay);
    if (err != DB_SUCCESS) {
      ib::warn(ER_IB_MSG_466)
          << "Failed to rename vector aux table " << old_aux_name << " -> "
          << new_aux_name << " err=" << static_cast<int>(err);
      return err;
    }

    /* Update the DD entry (dd::Table parent schema_id + dd::Tablespace
    file_name) — reuses dd_rename_fts_table since aux tables are
    DD-registered with the same shape. dict_sys mutex must be released
    around the DD client call. */
    if (!replay) {
      dict_table_t *aux = dict_table_check_if_in_cache_low(new_aux_name);
      ut_ad(aux != nullptr);
      if (aux != nullptr) {
        aux->acquire();
        dict_sys_mutex_exit();
        const bool ok = dd_rename_fts_table(aux, old_aux_name);
        dict_sys_mutex_enter();
        aux->release();
        if (!ok) {
          ib::warn(ER_IB_MSG_466)
              << "Failed to rename DD entry for vector aux " << old_aux_name;
          return DB_ERROR;
        }
      }
    }
  }
  return DB_SUCCESS;
}
