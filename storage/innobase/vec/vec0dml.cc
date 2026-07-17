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

/** @file vec/vec0dml.cc
Parser-free DML on vector-index auxiliary tables. See vec0dml.h for the
DEVIATION FROM FTS rationale (no fts_parse_sql / pars_mutex). */

#include "vec0dml.h"

#include <algorithm>
#include <limits>

#include "btr0pcur.h"
#include "dict0dict.h"
#include "lob0lob.h"
#include "mach0data.h"
#include "pars0pars.h"
#include "que0que.h"
#include "read0types.h"
#include "row0ins.h"
#include "row0mysql.h"
#include "row0upd.h"
#include "row0vers.h"
#include "trx0roll.h"
#include "vec0aux.h"

/* Aux table user-column ordinals, fixed by create_in_mem_vec_aux_table
(vec0aux.cc): id, vec, row_ref, level, neighbors. */
constexpr ulint VEC_AUX_COL_ID = 0;
constexpr ulint VEC_AUX_COL_VEC = 1;
constexpr ulint VEC_AUX_COL_ROW_REF = 2;
constexpr ulint VEC_AUX_COL_LEVEL = 3;
constexpr ulint VEC_AUX_COL_NEIGHBORS = 4;

void vec_aux_serialize_neighbors(
    const std::vector<std::vector<std::size_t>> &neighbors_by_level,
    std::vector<byte> &out) {
  ulint total = 4;
  for (const auto &lvl : neighbors_by_level) {
    total += 4 + 8 * lvl.size();
  }
  out.resize(total);

  byte *p = out.data();
  mach_write_to_4(p, static_cast<uint32_t>(neighbors_by_level.size()));
  p += 4;
  for (const auto &lvl : neighbors_by_level) {
    mach_write_to_4(p, static_cast<uint32_t>(lvl.size()));
    p += 4;
    for (std::size_t label : lvl) {
      mach_write_to_8(p, static_cast<uint64_t>(label));
      p += 8;
    }
  }
  ut_ad(p == out.data() + total);
}

bool vec_aux_deserialize_neighbors(
    const byte *data, ulint len,
    std::vector<std::vector<std::size_t>> &neighbors_by_level) {
  neighbors_by_level.clear();
  if (len < 4) {
    return true;
  }
  const byte *p = data;
  const byte *end = data + len;
  const uint32_t nlevels = mach_read_from_4(p);
  p += 4;
  neighbors_by_level.reserve(nlevels);
  for (uint32_t l = 0; l < nlevels; ++l) {
    if (p + 4 > end) {
      return true;
    }
    const uint32_t count = mach_read_from_4(p);
    p += 4;
    if (p + 8 * static_cast<ulint>(count) > end) {
      return true;
    }
    std::vector<std::size_t> labels;
    labels.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      labels.push_back(static_cast<std::size_t>(mach_read_from_8(p)));
      p += 8;
    }
    neighbors_by_level.push_back(std::move(labels));
  }
  return p != end;
}

ulint vec_row_ref_serialize(const dtuple_t *row, const dict_table_t *table,
                            byte *out) {
  const dict_index_t *clust = table->first_index();
  ut_a(clust != nullptr);

  byte *p = out;
  const ulint n_unique = dict_index_get_n_unique(clust);
  for (ulint i = 0; i < n_unique; ++i) {
    const dict_field_t *ifield = clust->get_field(i);
    const ulint col_no = dict_col_get_no(ifield->col);
    const dfield_t *df = dtuple_get_nth_field(row, col_no);
    if (dfield_is_null(df)) {
      return 0; /* PK fields are NOT NULL; treat as error */
    }
    const ulint len = dfield_get_len(df);
    if (p - out + len > VEC_AUX_ROW_REF_COL_LEN) {
      return 0;
    }
    memcpy(p, dfield_get_data(df), len);
    p += len;
  }
  return p - out;
}

/** Fill one user dfield of the aux row tuple with a heap-duplicated
value (the run loop may retry after lock waits; values must be stable). */
static void vec_aux_set_field(dtuple_t *tuple, ulint col_no, const void *data,
                              ulint len, mem_heap_t *heap) {
  dfield_t *df = dtuple_get_nth_field(tuple, col_no);
  if (data == nullptr) {
    dfield_set_null(df);
    return;
  }
  void *copy = mem_heap_dup(heap, data, len);
  dfield_set_data(df, copy, len);
}

dberr_t vec_aux_insert(trx_t *trx, dict_table_t *aux,
                       const vec_aux_row_t &row) {
  ut_a(trx != nullptr);
  ut_a(aux != nullptr);
  ut_a(row.vec != nullptr);
  ut_a(row.neighbors != nullptr || row.neighbors_len == 0);

  /* The aux column is TINYINT; the HNSW level is geometrically
  distributed and cannot plausibly reach 127, but never store a
  truncated level. */
  if (row.level < 0 || row.level > 127) {
    return DB_CORRUPTION;
  }

  mem_heap_t *heap = mem_heap_create(1024, UT_LOCATION_HERE);

  /* Mirror of row_get_prebuilt_insert_row + row_insert_for_mysql's run
  loop (row0mysql.cc), minus the prebuilt: build an INS_DIRECT node on a
  private heap, complete a query graph for it (pars_complete_graph_for_
  exec builds the fork/thr only — no SQL parser involved), fill the row,
  and drive row_ins_step with the standard error handling. */
  ins_node_t *node = ins_node_create(INS_DIRECT, aux, heap);

  dtuple_t *tuple = dtuple_create(heap, aux->get_n_cols());
  dict_table_copy_types(tuple, aux);
  ins_node_set_new_row(node, tuple);

  byte id_buf[8];
  mach_write_to_8(id_buf, row.id);
  vec_aux_set_field(tuple, VEC_AUX_COL_ID, id_buf, sizeof(id_buf), heap);
  vec_aux_set_field(tuple, VEC_AUX_COL_VEC, row.vec,
                    row.dims * sizeof(float), heap);
  vec_aux_set_field(tuple, VEC_AUX_COL_ROW_REF, row.row_ref, row.row_ref_len,
                    heap);
  const byte level_byte = static_cast<byte>(row.level);
  vec_aux_set_field(tuple, VEC_AUX_COL_LEVEL, &level_byte, 1, heap);
  vec_aux_set_field(tuple, VEC_AUX_COL_NEIGHBORS, row.neighbors,
                    row.neighbors_len, heap);

  que_thr_t *thr = pars_complete_graph_for_exec(node, trx, heap, nullptr);

  auto savept = trx_savept_take(trx);

  que_thr_move_to_run_state_for_mysql(thr, trx);

  /* Each call is its own mini-statement on the trx: take the IX table
  lock explicitly (cheap when already held by an earlier row of the same
  statement). */
  node->state = INS_NODE_SET_IX_LOCK;

  dberr_t err;
  for (;;) {
    thr->run_node = node;
    thr->prev_node = node;

    row_ins_step(thr);

    err = trx->error_state;
    if (err == DB_SUCCESS) {
      break;
    }

    que_thr_stop_for_mysql(thr);
    thr->lock_state = QUE_THR_LOCK_ROW;
    const bool was_lock_wait = row_mysql_handle_errors(&err, trx, thr, &savept);
    thr->lock_state = QUE_THR_LOCK_NOLOCK;

    if (!was_lock_wait) {
      mem_heap_free(heap);
      return err;
    }
    ut_ad(node->state == INS_NODE_INSERT_ENTRIES ||
          node->state == INS_NODE_ALLOC_ROW_ID);
  }

  que_thr_stop_for_mysql_no_error(thr, trx);
  mem_heap_free(heap);
  return DB_SUCCESS;
}

/** Shared body for the three targeted aux-row updates (neighbors,
tombstone, row_ref refresh): position on the PK, lock like a regular
UPDATE would, run one row_upd_step. Which columns land in the update
vector is driven by the arguments. */
static dberr_t vec_aux_update_row_low(trx_t *trx, dict_table_t *aux,
                                      uint64_t id, const byte *neighbors,
                                      ulint neighbors_len, bool set_neighbors,
                                      bool row_ref_null, const byte *row_ref,
                                      ulint row_ref_len) {
  ut_a(trx != nullptr);
  ut_a(aux != nullptr);
  ut_a(!set_neighbors || neighbors != nullptr || neighbors_len == 0);
  ut_a(!(row_ref_null && row_ref != nullptr));

  mem_heap_t *heap = mem_heap_create(1024, UT_LOCATION_HERE);
  dict_index_t *clust = aux->first_index();

  /* Standard update machinery — the same upd_node + row_upd_step every
  SQL UPDATE runs on. In a regular UPDATE the preceding row_search_mvcc
  read positions the cursor and takes the locks; we know the PK and
  skip the search, so we position and lock ourselves below.
  (Self-positioned-upd_node implementation reference: the FK-cascade
  code, row0ins.cc:1153; its run loop touches thr->prebuilt, which we
  don't have — hence a private loop.) */
  upd_node_t *node = row_create_update_node_for_mysql(aux, heap);

  /* Search tuple for the target row's PK. */
  dtuple_t *ref = dtuple_create(heap, 1);
  dict_index_copy_types(ref, clust, 1);
  byte id_buf[8];
  mach_write_to_8(id_buf, id);
  dfield_set_data(dtuple_get_nth_field(ref, 0), id_buf, sizeof(id_buf));

  que_thr_t *thr = pars_complete_graph_for_exec(node, trx, heap, nullptr);

  auto savept = trx_savept_take(trx);

  que_thr_move_to_run_state_for_mysql(thr, trx);

  /* Take the locks row_search_mvcc would have taken for a regular
  UPDATE: IX on the table, explicit X on the record (row_upd_clust_step
  asserts both via lock_trx_has_rec_x_lock). Position the cursor, take
  the locks, retry on lock waits with the standard
  row_mysql_handle_errors machinery. */
  mtr_t mtr;
  mem_heap_t *offset_heap = nullptr;
  for (;;) {
    mtr_start(&mtr);
    node->pcur->open_no_init(clust, ref, PAGE_CUR_LE, BTR_SEARCH_LEAF, 0, &mtr,
                             UT_LOCATION_HERE);
    const rec_t *rec = node->pcur->get_rec();
    if (!page_rec_is_user_rec(rec) ||
        node->pcur->get_low_match() < dict_index_get_n_unique(clust)) {
      mtr_commit(&mtr);
      que_thr_stop_for_mysql_no_error(thr, trx);
      if (offset_heap != nullptr) {
        mem_heap_free(offset_heap);
      }
      mem_heap_free(heap);
      return DB_RECORD_NOT_FOUND;
    }

    dberr_t lerr = lock_table(0, aux, LOCK_IX, thr);
    if (lerr == DB_SUCCESS) {
      ulint *offsets = rec_get_offsets(rec, clust, nullptr, ULINT_UNDEFINED,
                                       UT_LOCATION_HERE, &offset_heap);
      /* Not lock_clust_rec_modify_check_and_lock: that one uses
      lock_rec_lock(impl=true), which creates NO explicit lock when
      uncontended (the caller is expected to modify the record in the
      same mtr, making the lock implicit via the new trx id). We modify
      in a LATER mtr, so we need the explicit X lock a SELECT ... FOR
      UPDATE would take. */
      lerr = lock_clust_rec_read_check_and_lock(
          lock_duration_t::REGULAR, node->pcur->get_block(), rec, clust,
          offsets, SELECT_ORDINARY, LOCK_X, LOCK_REC_NOT_GAP, thr);
      if (lerr == DB_SUCCESS_LOCKED_REC) {
        lerr = DB_SUCCESS;
      }
    }

    if (lerr == DB_SUCCESS) {
      node->pcur->store_position(&mtr);
      mtr_commit(&mtr);
      break;
    }

    mtr_commit(&mtr);
    trx->error_state = lerr;
    que_thr_stop_for_mysql(thr);
    thr->lock_state = QUE_THR_LOCK_ROW;
    const bool was_lock_wait =
        row_mysql_handle_errors(&lerr, trx, thr, &savept);
    thr->lock_state = QUE_THR_LOCK_NOLOCK;
    if (!was_lock_wait) {
      if (offset_heap != nullptr) {
        mem_heap_free(offset_heap);
      }
      mem_heap_free(heap);
      return lerr;
    }
  }
  if (offset_heap != nullptr) {
    mem_heap_free(offset_heap);
  }

  /* One- or two-field update vector, per the caller's request. */
  upd_t *update = upd_create(2, heap);
  update->table = aux;
  ulint n_fields = 0;

  if (set_neighbors) {
    upd_field_t *uf = upd_get_nth_field(update, n_fields++);
    const dict_col_t *col = aux->get_col(VEC_AUX_COL_NEIGHBORS);
    upd_field_set_field_no(uf, dict_col_get_clust_pos(col, clust), clust);
    void *copy = neighbors_len != 0
                     ? mem_heap_dup(heap, neighbors, neighbors_len)
                     : nullptr;
    dfield_set_data(&uf->new_val, copy, neighbors_len);
    col->copy_type(dfield_get_type(&uf->new_val));
  }

  if (row_ref_null || row_ref != nullptr) {
    upd_field_t *uf = upd_get_nth_field(update, n_fields++);
    const dict_col_t *col = aux->get_col(VEC_AUX_COL_ROW_REF);
    upd_field_set_field_no(uf, dict_col_get_clust_pos(col, clust), clust);
    if (row_ref_null) {
      dfield_set_null(&uf->new_val);
    } else {
      dfield_set_data(&uf->new_val, mem_heap_dup(heap, row_ref, row_ref_len),
                      row_ref_len);
    }
    col->copy_type(dfield_get_type(&uf->new_val));
  }
  ut_a(n_fields > 0);

  update->n_fields = n_fields;
  node->update = update;
  node->update_n_fields = n_fields;
  node->cmpl_info = 0;
  node->state = UPD_NODE_UPDATE_CLUSTERED;

  dberr_t err;
  for (;;) {
    thr->run_node = node;
    thr->prev_node = node;

    row_upd_step(thr);

    err = trx->error_state;
    if (err == DB_SUCCESS) {
      break;
    }

    que_thr_stop_for_mysql(thr);
    thr->lock_state = QUE_THR_LOCK_ROW;
    const bool was_lock_wait = row_mysql_handle_errors(&err, trx, thr, &savept);
    thr->lock_state = QUE_THR_LOCK_NOLOCK;

    if (!was_lock_wait) {
      mem_heap_free(heap);
      return err;
    }
  }

  que_thr_stop_for_mysql_no_error(thr, trx);
  mem_heap_free(heap);
  return DB_SUCCESS;
}

dberr_t vec_aux_update_neighbors(trx_t *trx, dict_table_t *aux, uint64_t id,
                                 const byte *neighbors, ulint neighbors_len,
                                 bool row_ref_null) {
  return vec_aux_update_row_low(trx, aux, id, neighbors, neighbors_len,
                                true /* set_neighbors */, row_ref_null, nullptr,
                                0);
}

dberr_t vec_aux_tombstone(trx_t *trx, dict_table_t *aux, uint64_t id) {
  return vec_aux_update_row_low(trx, aux, id, nullptr, 0,
                                false /* set_neighbors */,
                                true /* row_ref_null */, nullptr, 0);
}

dberr_t vec_aux_update_row_ref(trx_t *trx, dict_table_t *aux, uint64_t id,
                               const byte *row_ref, ulint row_ref_len) {
  ut_a(row_ref != nullptr);
  return vec_aux_update_row_low(trx, aux, id, nullptr, 0,
                                false /* set_neighbors */,
                                false /* row_ref_null */, row_ref, row_ref_len);
}

/** Copy one (possibly externally stored) field of an aux clustered-index
record into a byte vector.
@return true on success */
static bool vec_aux_copy_field(trx_t *trx, const dict_index_t *clust,
                               const rec_t *rec, const ulint *offsets,
                               ulint clust_pos, mem_heap_t *heap,
                               const byte **data, ulint *len) {
  if (rec_offs_nth_extern(clust, offsets, clust_pos)) {
    size_t ver;
    *data = lob::btr_rec_copy_externally_stored_field(
        trx, clust, rec, offsets, dict_table_page_size(clust->table),
        clust_pos, len, &ver, false, heap);
    return *data != nullptr;
  }
  *data = rec_get_nth_field(clust, rec, offsets, clust_pos, len);
  return *len != UNIV_SQL_NULL;
}

dberr_t vec_base_max_idx_id(dict_table_t *base, uint64_t *max_id) {
  ut_a(base != nullptr);
  ut_a(max_id != nullptr);
  ut_a(base->vec_idx_id_col != ULINT_UNDEFINED);

  *max_id = 0;

  dict_index_t *clust = base->first_index();
  const ulint pos_id =
      dict_col_get_clust_pos(base->get_col(base->vec_idx_id_col), clust);

  mem_heap_t *offset_heap = nullptr;

  mtr_t mtr;
  mtr_start(&mtr);

  btr_pcur_t pcur;
  pcur.open_at_side(true /* left */, clust, BTR_SEARCH_LEAF, true, 0, &mtr);

  ulint n_scanned = 0;

  while (pcur.move_to_next_user_rec(&mtr) == DB_SUCCESS) {
    const rec_t *rec = pcur.get_rec();

    ulint *offsets = rec_get_offsets(rec, clust, nullptr, ULINT_UNDEFINED,
                                     UT_LOCATION_HERE, &offset_heap);

    /* Every record counts, delete-marked included: a purge-pending id
    is still an id that must never be reissued. No version-building —
    the callers run in quiesced (exclusive-MDL) contexts and the
    hidden column of any record version carries a consumed id. */
    ulint len;
    const byte *data = rec_get_nth_field(clust, rec, offsets, pos_id, &len);
    if (len == 8) {
      *max_id = std::max(*max_id, mach_read_from_8(data));
    }

    if (++n_scanned % 512 == 0) {
      pcur.store_position(&mtr);
      mtr_commit(&mtr);
      mtr_start(&mtr);
      pcur.restore_position(BTR_SEARCH_LEAF, &mtr, UT_LOCATION_HERE);
    }
  }

  pcur.close();
  mtr_commit(&mtr);

  if (offset_heap != nullptr) {
    mem_heap_free(offset_heap);
  }

  return DB_SUCCESS;
}

dberr_t vec_aux_load_rows(
    dict_table_t *aux, uint32_t dims, std::vector<vec_loaded_row_t> *rows,
    uint64_t *raw_max_id, bool *saw_invisible,
    std::vector<uint64_t> *dead_labels,
    std::vector<std::pair<uint64_t, std::string>> *row_refs) {
  ut_a(aux != nullptr);
  ut_a(rows != nullptr);
  ut_a(raw_max_id != nullptr);
  ut_a(saw_invisible != nullptr);

  rows->clear();
  *raw_max_id = 0;
  *saw_invisible = false;

  dict_index_t *clust = aux->first_index();

  /* Column positions in the clustered index record (PK first, then
  DB_TRX_ID/DB_ROLL_PTR, then the rest). */
  const ulint pos_vec =
      dict_col_get_clust_pos(aux->get_col(VEC_AUX_COL_VEC), clust);
  const ulint pos_row_ref =
      dict_col_get_clust_pos(aux->get_col(VEC_AUX_COL_ROW_REF), clust);
  const ulint pos_level =
      dict_col_get_clust_pos(aux->get_col(VEC_AUX_COL_LEVEL), clust);
  const ulint pos_neighbors =
      dict_col_get_clust_pos(aux->get_col(VEC_AUX_COL_NEIGHBORS), clust);

  /* A background transaction with a read view gives a consistent
  snapshot without blocking writers; shape borrowed from
  DDL_Log_Table::search_all (log0ddl.cc) plus MVCC. */
  trx_t *trx = trx_allocate_for_background();
  trx_start_internal_read_only(trx, UT_LOCATION_HERE);
  ReadView *view = trx_assign_read_view(trx);
  if (view == nullptr) {
    trx_commit_for_mysql(trx);
    trx_free_for_background(trx);
    return DB_OUT_OF_RESOURCES;
  }

  dberr_t err = DB_SUCCESS;
  mem_heap_t *offset_heap = nullptr;
  mem_heap_t *row_heap = mem_heap_create(2048, UT_LOCATION_HERE);
  mem_heap_t *vers_heap = nullptr;

  mtr_t mtr;
  mtr_start(&mtr);

  btr_pcur_t pcur;
  pcur.open_at_side(true /* left */, clust, BTR_SEARCH_LEAF, true, 0, &mtr);

  ulint n_scanned = 0;

  /* Note: move_to_next_user_rec returns dberr_t (DB_END_OF_INDEX at
  the end), not bool — both values are non-zero, so compare explicitly. */
  while (pcur.move_to_next_user_rec(&mtr) == DB_SUCCESS) {
    const rec_t *rec = pcur.get_rec();

    ulint *offsets = rec_get_offsets(rec, clust, nullptr, ULINT_UNDEFINED,
                                     UT_LOCATION_HERE, &offset_heap);

    /* raw_max_id spans every record, visible or not: the counter must
    never hand out an id that any (possibly not-yet-visible, e.g.
    XA-prepared) committed row already owns. */
    {
      ulint id_len;
      const byte *id_ptr = rec_get_nth_field(clust, rec, offsets, 0, &id_len);
      ut_ad(id_len == 8);
      *raw_max_id = std::max(*raw_max_id, mach_read_from_8(id_ptr));
    }

    const rec_t *vrec = rec;
    ulint *voffsets = offsets;

    const trx_id_t rec_trx_id = row_get_rec_trx_id(rec, clust, offsets);
    if (!view->changes_visible(rec_trx_id, aux->name)) {
      /* Build the visible version, if any. */
      rec_t *old_vers = nullptr;
      err = row_vers_build_for_consistent_read(rec, &mtr, clust, &voffsets,
                                               view, &offset_heap, vers_heap,
                                               &old_vers, nullptr, nullptr);
      if (err != DB_SUCCESS) {
        break;
      }
      *saw_invisible = true;
      if (old_vers == nullptr) {
        continue; /* no version visible to us */
      }
      vrec = old_vers;
    }

    if (rec_get_deleted_flag(vrec, dict_table_is_comp(aux))) {
      continue;
    }

    /* id (from the visible version — PK never changes, but be exact) */
    ulint id_len;
    const byte *id_ptr = rec_get_nth_field(clust, vrec, voffsets, 0, &id_len);
    const uint64_t id = mach_read_from_8(id_ptr);

    /* row_ref NULL = tombstone: not part of the graph. Committed
    neighbor lists may still reference it — loadIndex drops those
    edges (dangling-label tolerance). Live rows optionally emit their
    row_ref image for the label -> base-PK map the kNN path uses. */
    {
      ulint rr_len;
      const byte *rr =
          rec_get_nth_field(clust, vrec, voffsets, pos_row_ref, &rr_len);
      if (rr_len == UNIV_SQL_NULL) {
        if (dead_labels != nullptr) {
          dead_labels->push_back(id);
        }
        continue;
      }
      if (row_refs != nullptr) {
        if (rec_offs_nth_extern(clust, voffsets, pos_row_ref)) {
          const byte *data;
          ulint len;
          if (!vec_aux_copy_field(trx, clust, vrec, voffsets, pos_row_ref,
                                  row_heap, &data, &len)) {
            err = DB_CORRUPTION;
            break;
          }
          row_refs->emplace_back(
              id, std::string(reinterpret_cast<const char *>(data), len));
        } else {
          row_refs->emplace_back(
              id, std::string(reinterpret_cast<const char *>(rr), rr_len));
        }
      }
    }

    const byte *vec_data;
    ulint vec_len;
    const byte *nb_data;
    ulint nb_len;
    if (!vec_aux_copy_field(trx, clust, vrec, voffsets, pos_vec, row_heap,
                            &vec_data, &vec_len) ||
        !vec_aux_copy_field(trx, clust, vrec, voffsets, pos_neighbors,
                            row_heap, &nb_data, &nb_len)) {
      err = DB_CORRUPTION;
      break;
    }

    /* dims == 0 means "derive from the data": used by callers that
    have no SQL-layer Field_vector at hand (debug tooling). Rows must
    still be float-aligned and mutually consistent. */
    if (dims == 0) {
      if (vec_len == 0 || vec_len % sizeof(float) != 0) {
        err = DB_CORRUPTION;
        break;
      }
      dims = static_cast<uint32_t>(vec_len / sizeof(float));
    } else if (vec_len != dims * sizeof(float)) {
      err = DB_CORRUPTION;
      break;
    }

    ulint level_len;
    const byte *level_ptr =
        rec_get_nth_field(clust, vrec, voffsets, pos_level, &level_len);
    ut_ad(level_len == 1);
    const uint64_t level = static_cast<uint64_t>(*level_ptr);

    std::vector<float> vec_copy(dims);
    memcpy(vec_copy.data(), vec_data, vec_len);

    std::vector<std::vector<std::size_t>> neighbors;
    if (vec_aux_deserialize_neighbors(nb_data, nb_len, neighbors)) {
      err = DB_CORRUPTION;
      break;
    }

    rows->emplace_back(id, level, std::move(vec_copy), std::move(neighbors));

    mem_heap_empty(row_heap);

    /* Batch the mtr so a large aux table doesn't pin one mtr forever.
    Done AFTER processing the current record: restore_position parks the
    cursor on this (already-consumed) record — or its predecessor if it
    was purged meanwhile — so the loop's next move_to_next_user_rec
    advances correctly either way. */
    if (++n_scanned % 512 == 0) {
      pcur.store_position(&mtr);
      mtr_commit(&mtr);
      mtr_start(&mtr);
      pcur.restore_position(BTR_SEARCH_LEAF, &mtr, UT_LOCATION_HERE);
    }
  }

  pcur.close();
  mtr_commit(&mtr);

  if (offset_heap != nullptr) {
    mem_heap_free(offset_heap);
  }
  mem_heap_free(row_heap);

  trx_commit_for_mysql(trx);
  trx_free_for_background(trx);

  return err;
}
