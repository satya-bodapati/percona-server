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

#include <algorithm>

#include "extra/hnswlib/hnswlib.h"
#include "my_dbug.h"
#include "vec0dml.h"

#include <cstdio>
#include <cstring>

#include "current_thd.h"
#include "data0type.h"
#include "data0types.h"
#include "dict0boot.h"
#include "dict0dd.h"
#include "dict0dict.h"
#include "dict0mem.h"
#include "fts0fts.h"
#include "fts0priv.h"
#include "mach0data.h"
#include "row0mysql.h"
#include "trx0trx.h"
#include "univ.i"
#include "ut0new.h"
#include "vec0index.h"

const char *VEC_AUX_PREFIX = "vec_";

namespace {

/** Extract the flags2 bits an aux table should inherit from its parent —
file_per_table, encryption, temporary, plus DICT_TF2_VEC_AUX. Same shape
the FTS aux path uses (DICT_TF2_AUX over there); the two flags are
distinct so predicates can tell vector aux from FTS aux without parsing
the on-disk name. */
inline uint32_t aux_flags2_from_parent(const dict_table_t *parent) {
  return (parent->flags2 & DICT_TF2_USE_FILE_PER_TABLE) |
         (parent->flags2 & DICT_TF2_ENCRYPTION_FILE_PER_TABLE) |
         (parent->flags2 & DICT_TF2_TEMPORARY) | DICT_TF2_VEC_AUX;
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
                            Vec_index_type type, char *name_out,
                            size_t name_out_len, const char *suffix) {
  ut_a(parent != nullptr);
  ut_a(name_out != nullptr);
  ut_a(name_out_len >= MAX_FULL_NAME_LEN);

  const char *parent_name = parent->name.m_name;
  const size_t db_len = db_prefix_len(parent_name);

  const char *token = vec_index_token(type);
  /* '_' is the field separator — a token containing it would make the
  name unparseable (contract in vec0index.h). */
  ut_ad(strchr(token, '_') == nullptr);

  char table_id_str[FTS_AUX_MIN_TABLE_ID_LENGTH];
  int n = fts_write_object_id(parent->id, table_id_str);
  ut_a(n > 0);

  char index_id_str[FTS_AUX_MIN_TABLE_ID_LENGTH];
  n = fts_write_object_id(index_id, index_id_str);
  ut_a(n > 0);

  /* A member table appends its suffix ("_dead"); the main table's is
  the empty string, so both share one format. */
  ut_a(suffix != nullptr);
  const int written = snprintf(
      name_out, name_out_len, "%.*s%s%s_%s_%s%s", static_cast<int>(db_len),
      parent_name, VEC_AUX_PREFIX, token, table_id_str, index_id_str, suffix);
  ut_a(written > 0);
  ut_a(static_cast<size_t>(written) < name_out_len);
}

bool vec_aux_is_aux_table_name(const char *name) {
  /* DEVIATION FROM FTS: fts_is_aux_table_name (fts0fts.cc) validates
  the full <prefix><hex_id>_<suffix> pattern and so leaves names like
  "fts_data" available to users. We reserve the ENTIRE "vec_" prefix
  instead — deliberately broader: aux names embed table_id and
  index_id, so any user vec_* table could collide with a future
  computed aux name, and unlike FTS we have no suffix whitelist to
  disambiguate. Consequence: CREATE TABLE vec_anything is rejected
  (see the reservation gate in create_table_def). Callers that need
  the embedded ids (DD reload, purge parent lookup) MUST use
  vec_aux_parse_table_name and handle its failure — a prefix match
  alone does not guarantee a parseable name. */
  if (name == nullptr) return false;
  const char *slash = strchr(name, '/');
  const char *after_db = slash != nullptr ? slash + 1 : name;
  return strncmp(after_db, VEC_AUX_PREFIX, strlen(VEC_AUX_PREFIX)) == 0;
}

bool vec_aux_parse_table_name(const char *name, table_id_t *parent_id_out,
                              space_index_t *index_id_out,
                              Vec_index_type *type_out) {
  if (!vec_aux_is_aux_table_name(name)) return false;
  const char *slash = strchr(name, '/');
  const char *after_db = slash != nullptr ? slash + 1 : name;
  const char *token =
      after_db + strlen(VEC_AUX_PREFIX);  // "<type>_<parent_id>_<index_id>"

  /* The type token runs to the next '_' and must resolve in the
  registry — an unknown token means "reserved vec_ name
  that is not an aux table". */
  const char *token_end = strchr(token, '_');
  if (token_end == nullptr || token_end == token) return false;
  const Vector_index *impl =
      vec_index_by_name(token, static_cast<size_t>(token_end - token));
  if (impl == nullptr) return false;

  const char *tail = token_end + 1;  // "<parent_id>_<index_id>"
  table_id_t pid = 0;
  if (!fts_read_object_id(&pid, tail)) return false;
  const char *sep = strchr(tail, '_');
  if (sep == nullptr) return false;
  space_index_t iid = 0;
  if (!fts_read_object_id(&iid, sep + 1)) return false;
  /* A member table's name carries a trailing suffix after the index id
  (e.g. "..._dead"); fts_read_object_id stops at it, so the ids parse
  identically for the main table and its members. */

  if (parent_id_out != nullptr) *parent_id_out = pid;
  if (index_id_out != nullptr) *index_id_out = iid;
  if (type_out != nullptr) *type_out = impl->type();
  return true;
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
  /* No phy_pos plumbing needed: INSTANT ADD/DROP COLUMN is blocked on
  vec-indexed tables (see innobase_support_instant, mirrors FTS), so
  vec_idx_id can never live on a table with row_versions > 0. Shape
  matches fts_add_doc_id_column for the same reason. */
  dict_mem_table_add_col(
      table, heap, VEC_IDX_ID_COL_NAME, DATA_INT,
      dtype_form_prtype(DATA_NOT_NULL | DATA_UNSIGNED | DATA_BINARY_TYPE, 0),
      sizeof(uint64_t), false);
  DICT_TF2_FLAG_SET(table, DICT_TF2_VEC_HAS_IDX_ID);
  /* Remember the ordinal position so the INSERT path can locate the
  dfield slot in O(1) — same trick FTS uses with table->fts->doc_col. */
  table->vec_idx_id_col = table->n_def - 1;
}

void vec_stamp_idx_id(dict_table_t *table, dtuple_t *row, mem_heap_t *heap) {
  ut_a(table != nullptr);
  ut_a(row != nullptr);
  ut_a(heap != nullptr);
  if (!DICT_TF2_FLAG_IS_SET(table, DICT_TF2_VEC_HAS_IDX_ID)) {
    return;
  }
  ut_a(table->vec_idx_id_col != ULINT_UNDEFINED);
  ut_a(table->vec_idx_id_col < dtuple_get_n_fields(row));

  const uint64_t id = vec_assign_next_idx_id(table);

  /* Mirrors fts_create_doc_id: allocate the value on `heap` so it
  outlives this call, then point the dfield at it. Big-endian (mach
  format) so a clustered-index range scan on this column would order
  numerically — matches how FTS_DOC_ID is laid out. */
  uint64_t *buf = static_cast<uint64_t *>(mem_heap_alloc(heap, sizeof(*buf)));
  mach_write_to_8(reinterpret_cast<byte *>(buf), id);

  dfield_t *dfield = dtuple_get_nth_field(row, table->vec_idx_id_col);
  dfield_set_data(dfield, buf, sizeof(*buf));
}

uint64_t vec_assign_next_idx_id(dict_table_t *table) {
  ut_a(table != nullptr);
  ut_a(DICT_TF2_FLAG_IS_SET(table, DICT_TF2_VEC_HAS_IDX_ID));

  /* fetch_add returns the OLD value; +1 makes the first assignment 1. */
  const uint64_t id =
      table->vec_next_id.fetch_add(1, std::memory_order_acq_rel) + 1;

  /* Persist the advance, autoinc-style (PM_TABLE_VEC_IDX_ID dynamic
  metadata): the redo record makes the id durable the moment it is
  consumed, so a label can NEVER be reissued — not across restart, not
  across crash, and regardless of whether this id ever reaches the aux
  (NULL-vector rows and rolled-back inserts consume ids the aux-table
  maximum cannot see). This is what makes base-row vec_idx_id values
  unique for the lifetime of a table_id. The stamp runs outside any
  active mini-transaction, hence a dedicated one; the watermark check
  inside the log call keeps redo traffic to one record per NEW
  maximum. */
  mtr_t mtr;
  mtr.start();
  const bool persist = dict_table_vec_next_id_log(table, id, &mtr);
  mtr.commit();

  if (persist) {
    dict_table_persist_to_dd_table_buffer(table);
  }

  return id;
}

namespace {

/* --------------------------------------------------------------------
Per-TYPE aux table sets. One index owns a SET of aux tables, each
described by a suffix + fixed schema; the PK is always the leading n_pk
columns. Adding an index TYPE means adding its descriptors and one case
in vec_aux_table_set — no create/drop/rename/import path changes, and
the DD writer (dd_create_vec_aux_table) derives its entry from the
in-memory table these descriptors build. */

struct Vec_aux_col_def {
  const char *name;
  ulint mtype;
  ulint prtype;
  ulint len;
};

struct Vec_aux_table_def {
  const char *suffix; /* "" = the main table */
  ulint n_cols;
  const Vec_aux_col_def *cols;
  ulint n_pk; /* the PK = the first n_pk columns */
};

constexpr ulint VEC_AUX_BLOB_PRTYPE =
    (DATA_MTYPE_MAX << 16) | DATA_BINARY_TYPE | DATA_NOT_NULL;

/* hnsw H1 (design doc hnsw-aux-log-design.md §04): ONE ROW PER
   MUTATION. PK(label, ver): ver 0 is the birth row and alone carries
   the node's identity (vec, row_ref, level); ver > 0 rows are
   edge-list snapshots captured atomically under hnswlib's per-node
   link lock, so version order provably equals mutation order. Nothing
   is ever UPDATEd — appends of distinct keys cannot deadlock and
   cannot overwrite. The loader takes, per label, identity from ver 0
   and edges from the HIGHEST VISIBLE ver. */
const Vec_aux_col_def vec_hnsw_log_cols[] = {
    {"label", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, VEC_AUX_ID_COL_LEN},
    {"ver", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 4},
    /* vec/row_ref/level: ver-0 (birth) rows only — NULL on version rows */
    {"vec", DATA_BLOB, (DATA_MTYPE_MAX << 16) | DATA_BINARY_TYPE,
     VEC_AUX_VEC_COL_LEN},
    {"row_ref", DATA_BINARY, DATA_BINARY_TYPE, VEC_AUX_ROW_REF_COL_LEN},
    {"level", DATA_INT, 0, VEC_AUX_LEVEL_COL_LEN},
    {"neighbors", DATA_BLOB, VEC_AUX_BLOB_PRTYPE, VEC_AUX_NEIGHBORS_COL_LEN},
};

const Vec_aux_table_def vec_hnsw_defs[] = {
    {"", UT_ARR_SIZE(vec_hnsw_log_cols), vec_hnsw_log_cols, 2},
};

/** The aux table set for one index TYPE. hnsw is the only registered
TYPE; a second one adds its descriptors above and a case here. */
void vec_aux_table_set(Vec_index_type type, const Vec_aux_table_def **defs,
                       ulint *n_defs) {
  switch (type) {
    case Vec_index_type::HNSW:
      break;
  }
  *defs = vec_hnsw_defs;
  *n_defs = UT_ARR_SIZE(vec_hnsw_defs);
}

/** Allocate and fully populate the in-memory dict_table_t for one aux
table from its descriptor. */
dict_table_t *create_in_mem_vec_aux_table(const char *aux_name,
                                          const dict_table_t *parent,
                                          mem_heap_t *heap,
                                          const Vec_aux_table_def &def) {
  dict_table_t *t =
      dict_mem_table_create(aux_name, parent->space, def.n_cols, 0, 0,
                            parent->flags, aux_flags2_from_parent(parent));

  if (DICT_TF_HAS_SHARED_SPACE(parent->flags)) {
    ut_ad(parent->space == fil_space_get_id_by_name(parent->tablespace()));
    t->tablespace = mem_heap_strdup(t->heap, parent->tablespace);
  }
  if (DICT_TF_HAS_DATA_DIR(parent->flags)) {
    ut_ad(parent->data_dir_path != nullptr);
    t->data_dir_path = mem_heap_strdup(t->heap, parent->data_dir_path);
  }

  for (ulint i = 0; i < def.n_cols; ++i) {
    const Vec_aux_col_def &c = def.cols[i];
    dict_mem_table_add_col(t, heap, c.name, c.mtype, c.prtype, c.len, true);
  }

  return t;
}

}  // namespace

dberr_t vec_aux_create_one_table(trx_t *trx, const dict_table_t *parent,
                                 space_index_t index_id) {
  ut_a(trx != nullptr);
  ut_a(parent != nullptr);

  const Vec_aux_table_def *defs = nullptr;
  ulint n_defs = 0;
  vec_aux_table_set(Vec_index_type::HNSW, &defs, &n_defs);

  for (ulint d = 0; d < n_defs; ++d) {
    const Vec_aux_table_def &def = defs[d];

    char aux_name[MAX_FULL_NAME_LEN];
    vec_aux_get_table_name(parent, index_id, Vec_index_type::HNSW, aux_name,
                           sizeof(aux_name), def.suffix);

    mem_heap_t *heap = mem_heap_create(1024, UT_LOCATION_HERE);
    dict_table_t *aux =
        create_in_mem_vec_aux_table(aux_name, parent, heap, def);

    dberr_t err =
        row_create_table_for_mysql(aux, nullptr, nullptr, trx, nullptr);

    if (err == DB_SUCCESS) {
      dict_index_t *cidx =
          dict_mem_index_create(aux_name, "VEC_AUX_TABLE_PK", aux->space,
                                DICT_UNIQUE | DICT_CLUSTERED, def.n_pk);
      for (ulint k = 0; k < def.n_pk; ++k) {
        cidx->add_field(def.cols[k].name, 0, true);
      }

      const trx_dict_op_t saved_op = trx_get_dict_operation(trx);
      err = row_create_index_for_mysql(cidx, trx, nullptr, nullptr);
      trx->dict_operation = saved_op;
    }

    mem_heap_free(heap);

    if (err != DB_SUCCESS) {
      trx->error_state = err;
      ib::warn(ER_IB_MSG_465)
          << "Failed to create vector aux table " << aux_name;
      return err;
    }
  }
  return DB_SUCCESS;
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

bool vec_aux_create_dd_tables(dict_table_t *parent) {
  ut_a(parent != nullptr);

  /* DEVIATION FROM FTS: fts_create_index_dd_tables (fts0fts.cc) gates
  each iteration on `index->fill_dd` so a re-entrant CREATE-time call
  registers exactly the pending aux tables. Vec has no `fill_dd` per-
  index gate because PS-11264 currently allows at most one vector
  index per table (see dd::create_dd_table validation) — so the loop
  either finds zero vec indexes or exactly one, and idempotency isn't
  a concern. If phase 2 lifts the one-vec-index cap AND supports
  partial DD materialization, mirror fts's fill_dd gate here. */
  for (const dict_index_t *idx = UT_LIST_GET_FIRST(parent->indexes);
       idx != nullptr; idx = UT_LIST_GET_NEXT(indexes, idx)) {
    if (!idx->is_vector()) continue;

    char aux_name[MAX_FULL_NAME_LEN];
    vec_aux_get_table_name(parent, idx->id, Vec_index_type::HNSW, aux_name,
                           sizeof(aux_name));
    dict_table_t *aux = dd_table_open_on_name_in_mem(aux_name, false);
    ut_a(aux != nullptr);
    const bool ok = dd_create_vec_aux_table(parent, aux);
    dd_table_close(aux, nullptr, nullptr, false);
    if (!ok) return false;
  }
  return true;
}

dberr_t vec_aux_drop_one_table(trx_t *trx, const dict_table_t *parent,
                               space_index_t index_id) {
  ut_a(trx != nullptr);
  ut_a(parent != nullptr);

  char aux_name[MAX_FULL_NAME_LEN];
  vec_aux_get_table_name(parent, index_id, Vec_index_type::HNSW, aux_name,
                         sizeof(aux_name));

  const bool file_per_table = dict_table_is_file_per_table(parent);

  /* Open the aux with MDL before row_drop_table_for_mysql. Without
  this, row_drop_table_for_mysql's call into dd_table_open_on_name
  trips dictionary_client.cc:734 because the SQL layer never
  acquires MDL on the hidden aux. Mirrors fts_drop_table at
  fts0fts.cc:1301-1313. Callers of vec_aux_drop_one_table always
  hold dict_sys (parent row_drop_table_for_mysql, ALTER commit
  drop-index loop, error_handling), so pass dict_locked=true and
  let dd_table_open_on_name handle the release-around-MDL-acquire
  dance internally — same convention FTS uses. */
  THD *thd = current_thd;
  MDL_ticket *aux_mdl = nullptr;
  if (thd != nullptr) {
    dict_table_t *aux = dd_table_open_on_name(
        thd, &aux_mdl, aux_name, true,
        static_cast<dict_err_ignore_t>(DICT_ERR_IGNORE_INDEX_ROOT |
                                       DICT_ERR_IGNORE_CORRUPT));
    if (aux != nullptr) {
      dd_table_close(aux, thd, &aux_mdl, true);
    }
  }

  dberr_t err = row_drop_table_for_mysql(aux_name, trx, false, nullptr);
  if (err != DB_SUCCESS && err != DB_TABLE_NOT_FOUND) {
    ib::warn(ER_IB_MSG_466) << "Failed to drop vector aux table " << aux_name
                            << " err=" << static_cast<int>(err);
    return err;
  }

  /* row_drop_table_for_mysql only tears down dict_sys + the .ibd. The
  matching dd::Table + dd::Tablespace entries created by
  dd_create_vec_aux_table linger until we explicitly drop them; reuse
  dd_drop_fts_table for that, which is generic across aux-table kinds.
  dict_sys mutex must be released around the DD client call.

  DEVIATION FROM FTS: fts_drop_table drops the DD entry inline only
  when called with aux_vec == nullptr; on the DROP TABLE path it
  instead pushes the aux name into aux_vec and the caller
  (row_drop_table_for_mysql's funct_exit) drops the DD entries AFTER
  the parent drop trx commits. Vec has no aux_vec mode — the DD drop
  always happens here, potentially under an open parent-drop trx.
  Acceptable in phase 1 (empty aux, one aux per index, no partial-
  batch window); the aux_vec deferral is the upgrade path if
  PS-11300's crash-atomicity work needs it. */
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

void vec_aux_detach_tables(const dict_table_t *parent, bool dict_locked) {
  ut_a(parent != nullptr);

  if (!dict_locked) {
    dict_sys_mutex_enter();
  }

  for (const dict_index_t *idx = UT_LIST_GET_FIRST(parent->indexes);
       idx != nullptr; idx = UT_LIST_GET_NEXT(indexes, idx)) {
    if (!idx->is_vector()) continue;

    char aux_name[MAX_FULL_NAME_LEN];
    vec_aux_get_table_name(parent, idx->id, Vec_index_type::HNSW, aux_name,
                           sizeof(aux_name));

    dict_table_t *aux = dd_table_open_on_name_in_mem(aux_name, true);
    if (aux != nullptr) {
      if (!aux->can_be_evicted) {
        dict_table_allow_eviction(aux);
      }
      dd_table_close(aux, nullptr, nullptr, true);
    }
  }

  if (!dict_locked) {
    dict_sys_mutex_exit();
  }
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
    vec_aux_get_table_name(parent, idx->id, Vec_index_type::HNSW, old_aux_name,
                           sizeof(old_aux_name));

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

/* ------------------------------------------------------------------
In-memory HNSW graph state (PS-11300 phase 2a). */

/** Total bytes charged by all in-memory vector graphs. */
static std::atomic<uint64_t> vec_mem_total{0};

/** Budget for the above; bound to the innodb_hnsw_max_memory sysvar
in ha_innodb.cc (see the header comment for the FTS deviation). */
unsigned long long vec_hnsw_max_memory;

uint64_t vec_total_memory() { return vec_mem_total.load(); }

/** The hnsw runtime of `table`, or nullptr. This file allocated it
(vec_open), so this file alone may interpret the subtype. */
static inline vec_t *vec_hnsw(const dict_table_t *table) {
  return static_cast<vec_t *>(table->vec);
}

size_t vec_graph_size(const dict_table_t *table) {
  const vec_t *vec = vec_hnsw(table);
  return (vec != nullptr && vec->hnsw != nullptr)
             ? vec->hnsw->cur_element_count.load()
             : 0;
}

/** @return true if charging `extra` bytes would cross the budget */
static bool vec_mem_would_exceed(uint64_t extra) {
  return vec_mem_total.load() + extra > vec_hnsw_max_memory;
}

static void vec_mem_charge(vec_t *vec, uint64_t bytes) {
  vec->mem_used.fetch_add(bytes);
  vec_mem_total.fetch_add(bytes);
}

static void vec_mem_release_all(vec_t *vec) {
  vec_mem_total.fetch_sub(vec->mem_used.exchange(0));
}

/** Context passed through addPoint to the persistence callbacks. */
struct vec_addpoint_ctx_t {
  trx_t *trx; /* the trx the aux DML rides (user trx on the DML
              paths; the ALTER's trx during an index build) */
  vec_t *vec; /* companion for memory accounting, or nullptr during
              an index build (the build graph is private and
              discarded, only budget-checked) */
  uint32_t dims;
  dict_table_t *aux;   /* opened for this operation */
  const byte *row_ref; /* serialized base PK */
  ulint row_ref_len;
  dberr_t err; /* first callback/aux failure */
};

/** Open the aux table for a DML operation. No aux MDL: the caller holds
MDL on the BASE table (write_row / table open), and every DDL that can
drop the aux takes exclusive base MDL first — same protection argument
FTS relies on for its aux DML. Fast path is the dict cache; fall back to
the DD (with MDL) only when evicted. */
static dict_table_t *vec_aux_open_for_dml(dict_table_t *base,
                                          space_index_t index_id, THD *thd,
                                          MDL_ticket **mdl) {
  char aux_name[MAX_FULL_NAME_LEN];
  vec_aux_get_table_name(base, index_id, Vec_index_type::HNSW, aux_name,
                         sizeof(aux_name));

  *mdl = nullptr;
  dict_table_t *aux = dd_table_open_on_name_in_mem(aux_name, false);
  if (aux == nullptr && thd != nullptr) {
    aux =
        dd_table_open_on_name(thd, mdl, aux_name, false, DICT_ERR_IGNORE_NONE);
  }
  return aux;
}

static void vec_aux_close_for_dml(dict_table_t *aux, THD *thd,
                                  MDL_ticket **mdl) {
  dd_table_close(aux, *mdl != nullptr ? thd : nullptr, mdl, false);
}

/** The two persistence callbacks — the aux-table mirror of every graph
mutation (see PS-11300-design.md §6). Both record the FIRST failure in
ctx->err; addPoint's caller turns that into a statement error, and the
statement rollback undoes base row + aux rows together. */
static void vec_insert_cb(
    hnswlib::labeltype label, hnswlib::tableint internal_id [[maybe_unused]],
    uint32_t version, const void *data_point,
    const hnswlib::HierarchicalNSW<float>::NeighborLabelListsByLevel
        &neighbors_by_level,
    void *arg) {
  auto *ctx = static_cast<vec_addpoint_ctx_t *>(arg);
  if (ctx->err != DB_SUCCESS) {
    return;
  }

  std::vector<std::vector<std::size_t>> nbl;
  nbl.reserve(neighbors_by_level.size());
  for (const auto &lvl : neighbors_by_level) {
    nbl.emplace_back(lvl.begin(), lvl.end());
  }
  std::vector<byte> blob;
  vec_aux_serialize_neighbors(nbl, blob);

  vec_aux_row_t row;
  row.id = label;
  row.ver = version; /* birth rows are always version 0 (fork contract) */
  ut_ad(version == 0);
  row.vec = static_cast<const float *>(data_point);
  row.dims = ctx->dims;
  row.row_ref = ctx->row_ref;
  row.row_ref_len = ctx->row_ref_len;
  row.level = neighbors_by_level.empty()
                  ? 0
                  : static_cast<int>(neighbors_by_level.size() - 1);
  row.neighbors = blob.data();
  row.neighbors_len = blob.size();

  ctx->err = vec_aux_insert(ctx->trx, ctx->aux, row);

  if (ctx->err == DB_SUCCESS && row.level > 0 && ctx->vec != nullptr) {
    /* Upper-level link lists are allocated per node; charge them
    incrementally (the level-0 block was charged at capacity). */
    vec_mem_charge(ctx->vec,
                   ctx->vec->hnsw->size_links_per_element_ * row.level);
  }
}

static void vec_update_cb(
    hnswlib::labeltype label, hnswlib::tableint internal_id [[maybe_unused]],
    uint32_t version, const void *data_point [[maybe_unused]],
    const hnswlib::HierarchicalNSW<float>::NeighborLabelListsByLevel
        &neighbors_by_level,
    void *arg) {
  auto *ctx = static_cast<vec_addpoint_ctx_t *>(arg);
  if (ctx->err != DB_SUCCESS) {
    return;
  }

  std::vector<std::vector<std::size_t>> nbl;
  nbl.reserve(neighbors_by_level.size());
  for (const auto &lvl : neighbors_by_level) {
    nbl.emplace_back(lvl.begin(), lvl.end());
  }
  std::vector<byte> blob;
  vec_aux_serialize_neighbors(nbl, blob);

  /* H1: a rewired neighbor persists as an APPEND of (label, ver,
  neighbors) — never an UPDATE of a shared row. The snapshot and its
  version were captured atomically under the node's link lock, so the
  loader's highest-visible-ver rule reconstructs mutation order no
  matter how transactions commit or roll back. A version row whose
  label's birth row was rolled back is a harmless orphan: the loader
  finds no ver-0 identity and skips the label entirely. */
  ut_ad(version > 0);
  vec_aux_row_t row;
  row.id = label;
  row.ver = version;
  row.vec = nullptr;
  row.dims = 0;
  row.row_ref = nullptr;
  row.row_ref_len = 0;
  row.level = 0;
  row.neighbors = blob.data();
  row.neighbors_len = blob.size();

  ctx->err = vec_aux_insert(ctx->trx, ctx->aux, row);
}

vec_t *vec_open(dict_table_t *table, const Vector_index *impl,
                uint16_t field_no, uint32_t dims, int M, int ef_construction) {
  ut_a(table != nullptr);
  ut_a(impl != nullptr);

  if (table->vec != nullptr) {
    return vec_hnsw(table);
  }

  dict_sys_mutex_enter();
  if (table->vec == nullptr) {
    const dict_index_t *vec_index = nullptr;
    for (const dict_index_t *idx = table->first_index(); idx != nullptr;
         idx = idx->next()) {
      if (idx->is_vector()) {
        vec_index = idx;
        break;
      }
    }
    ut_a(vec_index != nullptr);

    auto *vec = ut::new_withkey<vec_t>(ut::make_psi_memory_key(mem_key_other));
    vec->table = table;
    vec->impl = impl;
    vec->index_id = vec_index->id;
    vec->field_no = field_no;
    vec->dims = dims;
    vec->M = M;
    vec->ef_construction = ef_construction;
    vec->loaded = false;
    vec->stale = false;
    vec->space = nullptr;
    vec->hnsw = nullptr;
    vec->mem_used = 0;
    rw_lock_create(vec_index_rw_lock_key, &vec->latch, LATCH_ID_VEC_INDEX);
    table->vec = vec;
  }
  dict_sys_mutex_exit();
  return vec_hnsw(table);
}

/** Build the graph from the aux table. Caller holds vec->latch in X. */
static dberr_t vec_load_locked(vec_t *vec, THD *thd) {
  ut_ad(rw_lock_own(&vec->latch, RW_LOCK_X));

  dict_table_t *table = vec->table;

  MDL_ticket *mdl = nullptr;
  dict_table_t *aux = vec_aux_open_for_dml(table, vec->index_id, thd, &mdl);
  if (aux == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  std::vector<vec_loaded_row_t> rows;
  uint64_t raw_max_id = 0;
  bool saw_invisible = false;
  std::vector<std::pair<uint64_t, std::string>> row_refs;
  dberr_t err = vec_aux_load_rows(aux, vec->dims, &rows, &raw_max_id,
                                  &saw_invisible, nullptr, &row_refs);
  vec_aux_close_for_dml(aux, thd, &mdl);
  if (err != DB_SUCCESS) {
    return err;
  }

  /* Replace any previous graph (exception-fallback reload). */
  if (vec->hnsw != nullptr) {
    delete vec->hnsw;
    vec->hnsw = nullptr;
  }
  if (vec->space != nullptr) {
    delete vec->space;
    vec->space = nullptr;
  }
  vec_mem_release_all(vec);

  const size_t max_elements = std::max<size_t>(1024, 2 * rows.size());

  try {
    vec->space = new hnswlib::L2Space(vec->dims);
    vec->hnsw = new hnswlib::HierarchicalNSW<float>(
        vec->space, max_elements, vec->M, vec->ef_construction);
    /* vec_loaded_row_t and hnswlib::VecAuxLoadedRowTuple are the same
    std::tuple type by construction (vec0dml.h) — pass through. */
    vec->hnsw->loadIndex(rows);
  } catch (const std::exception &e) {
    ib::warn() << "vec_load: graph construction failed for "
               << table->name.m_name << " (dims=" << vec->dims
               << " rows=" << rows.size() << " max_elements=" << max_elements
               << "): " << e.what();
    delete vec->hnsw;
    vec->hnsw = nullptr;
    delete vec->space;
    vec->space = nullptr;
    return DB_OUT_OF_MEMORY;
  }

  vec->hnsw->setAddPointInsertCallback(vec_insert_cb);
  vec->hnsw->setAddPointUpdateCallback(vec_update_cb);

  {
    std::lock_guard<std::mutex> g(vec->row_ref_mutex);
    vec->row_ref_map.clear();
    for (auto &lr : row_refs) {
      vec->row_ref_map.emplace(lr.first, std::move(lr.second));
    }
  }

  /* Level-0 block charged at capacity; upper-level lists per node. */
  uint64_t bytes =
      static_cast<uint64_t>(max_elements) * vec->hnsw->size_data_per_element_;
  for (const auto &row : rows) {
    bytes += vec->hnsw->size_links_per_element_ * std::get<1>(row);
  }

  if (vec_mem_would_exceed(bytes)) {
    ib::warn() << "Vector index graph for " << table->name.m_name << " needs "
               << bytes << " bytes but innodb_hnsw_max_memory is "
               << vec_hnsw_max_memory << " (in use: " << vec_mem_total.load()
               << "). Not loading; raise the limit to enable inserts.";
    delete vec->hnsw;
    vec->hnsw = nullptr;
    delete vec->space;
    vec->space = nullptr;
    return DB_OUT_OF_MEMORY;
  }

  vec_mem_charge(vec, bytes);

  /* Counter safety net only: the authoritative restore is the
  PM_TABLE_VEC_IDX_ID dynamic metadata applied at dict load, which
  covers ids the aux never saw (NULL-vector rows, rolled-back
  inserts). The aux maximum can still exceed it in one legal case —
  metadata written by a pre-persistence build — so take the max
  rather than overwrite. */
  uint64_t cur = table->vec_next_id.load();
  while (cur < raw_max_id &&
         !table->vec_next_id.compare_exchange_weak(cur, raw_max_id)) {
  }

  vec->loaded = true;
  vec->stale = false;

  DBUG_EXECUTE_IF("vec_load_log",
                  ib::info()
                      << "vec_load: table " << table->name.m_name
                      << " rows=" << rows.size() << " max_id=" << raw_max_id
                      << " invisible=" << (saw_invisible ? 1 : 0););

  return DB_SUCCESS;
}

dberr_t vec_load(dict_table_t *table, THD *thd) {
  vec_t *vec = vec_hnsw(table);
  ut_a(vec != nullptr);

  rw_lock_x_lock(&vec->latch, UT_LOCATION_HERE);
  dberr_t err = DB_SUCCESS;
  if (!vec->loaded || vec->stale.load()) {
    err = vec_load_locked(vec, thd);
  }
  rw_lock_x_unlock(&vec->latch);
  return err;
}

dberr_t vec_insert_point(trx_t *trx, dict_table_t *table, THD *thd, uint64_t id,
                         const float *vec_data, const byte *row_ref,
                         ulint row_ref_len) {
  vec_t *vec = vec_hnsw(table);
  ut_a(vec != nullptr);
  ut_a(vec_data != nullptr);

  MDL_ticket *mdl = nullptr;
  dict_table_t *aux = vec_aux_open_for_dml(table, vec->index_id, thd, &mdl);
  if (aux == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  /* Terminal past the budget: per-node link charges can cross the
  limit without a resize, so gate every insert, not just growth. */
  if (vec_mem_would_exceed(0)) {
    vec_aux_close_for_dml(aux, thd, &mdl);
    return DB_OUT_OF_MEMORY;
  }

  vec_addpoint_ctx_t ctx;
  ctx.trx = trx;
  ctx.vec = vec;
  ctx.dims = vec->dims;
  ctx.aux = aux;
  ctx.row_ref = row_ref;
  ctx.row_ref_len = row_ref_len;
  ctx.err = DB_SUCCESS;

  rw_lock_s_lock(&vec->latch, UT_LOCATION_HERE);

  /* Exception-fallback reload (rare; see vec_t::stale). */
  if (vec->stale.load() || !vec->loaded) {
    rw_lock_s_unlock(&vec->latch);
    dberr_t lerr = vec_load(table, thd);
    if (lerr != DB_SUCCESS) {
      vec_aux_close_for_dml(aux, thd, &mdl);
      return lerr;
    }
    rw_lock_s_lock(&vec->latch, UT_LOCATION_HERE);
  }

  /* Grow capacity before it runs out; resizeIndex is the one hnswlib
  operation that is NOT safe against concurrent addPoint — X latch. */
  while (vec->hnsw->cur_element_count.load() >= vec->hnsw->max_elements_) {
    rw_lock_s_unlock(&vec->latch);
    rw_lock_x_lock(&vec->latch, UT_LOCATION_HERE);
    if (vec->hnsw->cur_element_count.load() >= vec->hnsw->max_elements_) {
      const size_t old_max = vec->hnsw->max_elements_;
      const size_t new_max = old_max * 2;
      const uint64_t extra = static_cast<uint64_t>(new_max - old_max) *
                             vec->hnsw->size_data_per_element_;
      if (vec_mem_would_exceed(extra)) {
        rw_lock_x_unlock(&vec->latch);
        vec_aux_close_for_dml(aux, thd, &mdl);
        return DB_OUT_OF_MEMORY;
      }
      try {
        vec->hnsw->resizeIndex(new_max);
      } catch (...) {
        rw_lock_x_unlock(&vec->latch);
        vec_aux_close_for_dml(aux, thd, &mdl);
        return DB_OUT_OF_MEMORY;
      }
      vec_mem_charge(vec, extra);
    }
    rw_lock_x_unlock(&vec->latch);
    rw_lock_s_lock(&vec->latch, UT_LOCATION_HERE);
  }

  /* Captured BEFORE the aux DML: a rollback undoes this point's aux
  rows iff it rolls back to (at most) this undo number, and then the
  tracking entry below inverts the graph side too. */
  const undo_no_t undo_mark = trx->undo_no;

  dberr_t err = DB_SUCCESS;
  bool node_added = false;
  try {
    vec->hnsw->addPoint(vec_data, id, false, &ctx);
    node_added = true;
    err = ctx.err;
  } catch (...) {
    /* Mid-flight failure can leave a half-linked node — the one
    consumer of the stale+reload fallback (design §7). */
    vec->stale.store(true);
    err = DB_OUT_OF_MEMORY;
  }

  rw_lock_s_unlock(&vec->latch);
  vec_aux_close_for_dml(aux, thd, &mdl);

  /* Track the node even when a callback failed (err != DB_SUCCESS):
  the statement will fail and its rollback removes the aux rows; the
  tracking entry is what marks the in-memory node deleted then. */
  if (node_added) {
    {
      std::lock_guard<std::mutex> g(vec->row_ref_mutex);
      vec->row_ref_map[id] =
          std::string(reinterpret_cast<const char *>(row_ref), row_ref_len);
    }
    vec_trx_record(trx, table, id, vec_trx_op_type::ADDED, undo_mark);
  }

  return err;
}

dberr_t vec_delete_point(trx_t *trx, dict_table_t *table, THD *thd,
                         uint64_t label) {
  vec_t *vec = vec_hnsw(table);
  ut_a(vec != nullptr);

  MDL_ticket *mdl = nullptr;
  dict_table_t *aux = vec_aux_open_for_dml(table, vec->index_id, thd, &mdl);
  if (aux == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  rw_lock_s_lock(&vec->latch, UT_LOCATION_HERE);
  if (vec->stale.load() || !vec->loaded) {
    rw_lock_s_unlock(&vec->latch);
    dberr_t lerr = vec_load(table, thd);
    if (lerr != DB_SUCCESS) {
      vec_aux_close_for_dml(aux, thd, &mdl);
      return lerr;
    }
    rw_lock_s_lock(&vec->latch, UT_LOCATION_HERE);
  }

  const undo_no_t undo_mark = trx->undo_no;

  dberr_t err = vec_aux_tombstone(trx, aux, label);

  bool marked = false;
  if (err == DB_SUCCESS) {
    try {
      vec->hnsw->markDelete(label);
      marked = true;
    } catch (...) {
      /* Label not in the graph: a reload since the row was inserted
      (tombstones are skipped at load, or an exception reload). The
      aux tombstone above still stands. */
    }
  }

  rw_lock_s_unlock(&vec->latch);
  vec_aux_close_for_dml(aux, thd, &mdl);

  if (marked) {
    vec_trx_record(trx, table, label, vec_trx_op_type::MARKED, undo_mark);
  }

  return err;
}

dberr_t vec_refresh_row_ref(trx_t *trx, dict_table_t *table, THD *thd,
                            uint64_t label, const byte *row_ref,
                            ulint row_ref_len) {
  vec_t *vec = vec_hnsw(table);
  ut_a(vec != nullptr);

  MDL_ticket *mdl = nullptr;
  dict_table_t *aux = vec_aux_open_for_dml(table, vec->index_id, thd, &mdl);
  if (aux == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  const undo_no_t undo_mark = trx->undo_no;

  dberr_t err = vec_aux_update_row_ref(trx, aux, label, row_ref, row_ref_len);

  if (err == DB_SUCCESS) {
    /* Repoint the in-memory map entry. The undo log restores the aux
    column on rollback; the REFRESHED tracking entry restores the map
    (only when an entry existed — an unloaded graph has no map, and
    the next load rebuilds it undo-consistently from the aux). */
    std::lock_guard<std::mutex> g(vec->row_ref_mutex);
    auto it = vec->row_ref_map.find(label);
    if (it != vec->row_ref_map.end()) {
      byte old_ref[8];
      const ulint old_len =
          std::min<size_t>(it->second.size(), sizeof(old_ref));
      memcpy(old_ref, it->second.data(), old_len);
      it->second.assign(reinterpret_cast<const char *>(row_ref), row_ref_len);
      vec_trx_record(trx, table, label, vec_trx_op_type::REFRESHED, undo_mark,
                     old_ref, old_len);
    }
  }

  vec_aux_close_for_dml(aux, thd, &mdl);
  return err;
}

uint64_t vec_get_idx_id_from_rec(const dict_table_t *table, const rec_t *rec,
                                 const dict_index_t *index) {
  ulint offsets_[REC_OFFS_NORMAL_SIZE];
  ulint *offsets = offsets_;
  mem_heap_t *heap = nullptr;

  ut_a(table->vec_idx_id_col != ULINT_UNDEFINED);
  ut_ad(index->is_clustered());

  rec_offs_init(offsets_);
  offsets = rec_get_offsets(rec, index, offsets, ULINT_UNDEFINED,
                            UT_LOCATION_HERE, &heap);

  const ulint col_no = index->get_col_pos(table->vec_idx_id_col);
  ut_ad(col_no != ULINT_UNDEFINED);

  ulint len;
  const byte *data = rec_get_nth_field(index, rec, offsets, col_no, &len);
  ut_a(len == 8);
  const uint64_t id = mach_read_from_8(data);

  if (heap != nullptr) {
    mem_heap_free(heap);
  }
  return id;
}

void vec_trx_record(trx_t *trx, dict_table_t *table, uint64_t label,
                    vec_trx_op_type type, undo_no_t undo_no,
                    const byte *old_ref, ulint old_ref_len) {
  ut_a(trx != nullptr);
  ut_a(old_ref_len <= sizeof(vec_trx_op_t::old_ref));

  if (trx->vec_ops == nullptr) {
    trx->vec_ops =
        ut::new_withkey<vec_trx_ops_t>(ut::make_psi_memory_key(mem_key_other));
  }
  vec_trx_op_t op{table, label, type, undo_no, {}, old_ref_len};
  if (old_ref_len != 0) {
    memcpy(op.old_ref, old_ref, old_ref_len);
  }
  trx->vec_ops->ops.push_back(op);
}

void vec_trx_rollback(trx_t *trx, const trx_savept_t *savept) {
  if (trx->vec_ops == nullptr) {
    return;
  }

  const undo_no_t limit = savept != nullptr ? savept->least_undo_no : 0;
  auto &ops = trx->vec_ops->ops;

  /* Entries are appended in undo_no order, so the rolled-back ones
  are a suffix; invert newest-first, mirroring the undo pass. */
  while (!ops.empty() && ops.back().undo_no >= limit) {
    const vec_trx_op_t op = ops.back();
    ops.pop_back();

    vec_t *vec = vec_hnsw(op.table);
    if (vec == nullptr) {
      continue;
    }

    if (op.type == vec_trx_op_type::REFRESHED) {
      /* Map-only inversion; the aux column is restored by the undo
      log. No latch: the map has its own mutex. */
      std::lock_guard<std::mutex> g(vec->row_ref_mutex);
      auto it = vec->row_ref_map.find(op.label);
      if (it != vec->row_ref_map.end()) {
        it->second.assign(reinterpret_cast<const char *>(op.old_ref),
                          op.old_ref_len);
      }
      continue;
    }

    rw_lock_s_lock(&vec->latch, UT_LOCATION_HERE);
    if (vec->loaded && vec->hnsw != nullptr) {
      try {
        if (op.type == vec_trx_op_type::ADDED) {
          vec->hnsw->markDelete(op.label);
        } else {
          vec->hnsw->unmarkDelete(op.label);
        }
      } catch (...) {
        /* Label unknown: a stale-exception reload rebuilt the graph
        from committed rows in between, which already excluded this
        uncommitted node. Nothing to invert. */
      }
    }
    rw_lock_s_unlock(&vec->latch);
  }

  if (savept == nullptr) {
    ut_ad(ops.empty());
    vec_trx_free(trx);
  }
}

void vec_trx_free(trx_t *trx) {
  if (trx->vec_ops != nullptr) {
    ut::delete_(trx->vec_ops);
    trx->vec_ops = nullptr;
  }
}

namespace {
/** hnswlib filter dropping already-returned labels (resume). */
class Vec_exclude_filter : public hnswlib::BaseFilterFunctor {
 public:
  explicit Vec_exclude_filter(const std::unordered_set<uint64_t> &excluded)
      : m_excluded(excluded) {}
  bool operator()(hnswlib::labeltype label) override {
    return m_excluded.find(label) == m_excluded.end();
  }

 private:
  const std::unordered_set<uint64_t> &m_excluded;
};
}  // namespace

dberr_t vec_knn_search(dict_table_t *table, THD *thd, const float *query,
                       uint32_t dims, size_t k, size_t ef,
                       std::vector<vec_knn_hit_t> *hits,
                       const std::unordered_set<uint64_t> *exclude) {
  ut_a(hits != nullptr);
  hits->clear();

  vec_t *vec = vec_hnsw(table);
  if (vec == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }
  if (dims != 0 && dims != vec->dims) {
    return DB_CORRUPTION;
  }

  rw_lock_s_lock(&vec->latch, UT_LOCATION_HERE);
  if (vec->stale.load() || !vec->loaded) {
    rw_lock_s_unlock(&vec->latch);
    dberr_t lerr = vec_load(table, thd);
    if (lerr != DB_SUCCESS) {
      return lerr;
    }
    rw_lock_s_lock(&vec->latch, UT_LOCATION_HERE);
  }

  const size_t n = vec->hnsw->cur_element_count.load();
  if (n != 0 && k != 0) {
    /* hnswlib floors its search width at max(ef_, k): widening k to
    max(k, ef) makes `ef` a per-query recall knob without mutating the
    shared graph's ef_ (which would race between sessions). */
    const size_t kq = std::min(std::max(k, ef), n);
    try {
      static const std::unordered_set<uint64_t> s_no_excluded;
      Vec_exclude_filter filter(exclude != nullptr ? *exclude : s_no_excluded);
      auto found = vec->hnsw->searchKnnCloserFirst(
          query, kq,
          exclude != nullptr && !exclude->empty() ? &filter : nullptr);
      std::lock_guard<std::mutex> g(vec->row_ref_mutex);
      for (const auto &cand : found) {
        if (hits->size() >= k) {
          break;
        }
        vec_knn_hit_t hit;
        hit.label = cand.second;
        hit.dist = cand.first;
        auto it = vec->row_ref_map.find(hit.label);
        if (it != vec->row_ref_map.end()) {
          hit.row_ref = it->second;
        }
        hits->push_back(std::move(hit));
      }
    } catch (...) {
      rw_lock_s_unlock(&vec->latch);
      return DB_OUT_OF_MEMORY;
    }
  }
  rw_lock_s_unlock(&vec->latch);
  return DB_SUCCESS;
}

dberr_t vec_build_index(trx_t *trx, dict_table_t *table,
                        const dict_index_t *vec_index, uint32_t dims, int M,
                        int ef_construction, THD *thd) {
  ut_a(trx != nullptr);
  ut_a(vec_index != nullptr);
  ut_a(dims != 0);

  std::vector<vec_base_row_t> rows;
  dberr_t err = vec_base_collect_rows(trx, table, vec_index, dims, &rows);
  if (err != DB_SUCCESS) {
    return err;
  }
  if (rows.empty()) {
    return DB_SUCCESS;
  }

  MDL_ticket *mdl = nullptr;
  dict_table_t *aux = vec_aux_open_for_dml(table, vec_index->id, thd, &mdl);
  if (aux == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  hnswlib::L2Space *space = nullptr;
  hnswlib::HierarchicalNSW<float> *hnsw = nullptr;

  const size_t max_elements = std::max<size_t>(1024, rows.size());

  try {
    space = new hnswlib::L2Space(dims);
    hnsw = new hnswlib::HierarchicalNSW<float>(space, max_elements, M,
                                               ef_construction);
  } catch (...) {
    delete hnsw;
    delete space;
    vec_aux_close_for_dml(aux, thd, &mdl);
    return DB_OUT_OF_MEMORY;
  }

  /* Budget check only — no charge: the graph is private and discarded
  below; the durable footprint is the aux rows, and the graph a later
  first access loads is charged by vec_load as usual. */
  if (vec_mem_would_exceed(static_cast<uint64_t>(max_elements) *
                           hnsw->size_data_per_element_)) {
    ib::warn() << "Vector index build for " << table->name.m_name
               << " refused: it needs more than the remaining"
                  " innodb_hnsw_max_memory budget.";
    delete hnsw;
    delete space;
    vec_aux_close_for_dml(aux, thd, &mdl);
    return DB_OUT_OF_MEMORY;
  }

  vec_addpoint_ctx_t ctx;
  ctx.trx = trx;
  ctx.vec = nullptr;
  ctx.dims = dims;
  ctx.aux = aux;
  ctx.err = DB_SUCCESS;

  hnsw->setAddPointInsertCallback(vec_insert_cb);
  hnsw->setAddPointUpdateCallback(vec_update_cb);

#ifdef UNIV_DEBUG
  std::unordered_set<uint64_t> seen;
#endif /* UNIV_DEBUG */

  for (const auto &row : rows) {
    const uint64_t label = std::get<0>(row);
    ut_ad(seen.insert(label).second); /* base-id uniqueness invariant */

    ctx.row_ref = std::get<2>(row).data();
    ctx.row_ref_len = std::get<2>(row).size();

    try {
      hnsw->addPoint(std::get<1>(row).data(), label, false, &ctx);
    } catch (...) {
      err = DB_OUT_OF_MEMORY;
      break;
    }
    if (ctx.err != DB_SUCCESS) {
      /* A duplicate label surfaces here as the aux PK dup-key —
      impossible post counter-persistence, but never silent. */
      err = ctx.err;
      break;
    }
  }

  delete hnsw;
  delete space;
  vec_aux_close_for_dml(aux, thd, &mdl);

  return err;
}

dberr_t vec_aux_recreate_after_import(dict_table_t *table, trx_t *trx) {
  const dict_index_t *vec_index = nullptr;
  for (const dict_index_t *idx = table->first_index(); idx != nullptr;
       idx = idx->next()) {
    if (idx->is_vector()) {
      vec_index = idx;
      break;
    }
  }
  if (vec_index == nullptr) {
    return DB_SUCCESS;
  }

  /* Any graph state predates the import and is meaningless now —
  free it. The companion is re-created lazily from the SQL layer
  (innobase_vec_open_from_sql_layer) on the next open/insert/search —
  the dict object may be evicted at ALTER end anyway. */
  vec_close(table);

  trx_start_if_not_started(trx, true, UT_LOCATION_HERE);
  trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);

  row_mysql_lock_data_dictionary(trx, UT_LOCATION_HERE);
  dict_sys_mutex_exit();
  dberr_t err = vec_aux_create_one_table(trx, table, vec_index->id);
  dict_sys_mutex_enter();
  row_mysql_unlock_data_dictionary(trx);

  if (err == DB_SUCCESS) {
    /* Fresh aux — but NOT fresh labels: the imported base rows carry
    their source-stamped ids in the hidden column (it travels inside
    the .ibd), and base-id uniqueness (the counter-persistence
    invariant) requires new assignments to stay above every one of
    them. Re-seed from a clustered scan; the DISCARD-reassigned
    table_id orphaned the old dynamic-metadata row, so reset the
    watermark first and re-log the seed so it survives restart. */
    uint64_t max_id = 0;
    err = vec_base_max_idx_id(table, &max_id);
    if (err == DB_SUCCESS) {
      table->vec_next_id.store(max_id);
      table->vec_next_id_persisted.store(0);
      if (max_id != 0) {
        mtr_t mtr;
        mtr.start();
        const bool persist = dict_table_vec_next_id_log(table, max_id, &mtr);
        mtr.commit();
        if (persist) {
          dict_table_persist_to_dd_table_buffer(table);
        }
      }
    }
  }

  return err;
}

void vec_close(dict_table_t *table) {
  vec_t *vec = vec_hnsw(table);
  if (vec == nullptr) {
    return;
  }
  table->vec = nullptr;

  delete vec->hnsw;
  delete vec->space;
  vec_mem_release_all(vec);
  /* No rw_lock_free: that is the in-place ~rw_lock_t call for structs
  whose destructor never runs (the FTS mem-heap pattern). vec_t is
  deleted as a C++ object, so the member destructor below does the
  teardown — calling both trips rw_lock_validate. */
  ut::delete_(vec);
}
