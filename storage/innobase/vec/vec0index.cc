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

/** @file vec/vec0index.cc
The HNSW implementation of the Vector_index seam (SPANN plan, R1):
a stateless forwarding shim over the vec0aux free functions. All
per-index state stays in dict_table_t::vec; rollback tracking stays
INSIDE vec_insert_point / vec_delete_point — an implementation detail
this seam deliberately does not expose (spann needs none). */

#include "vec0index.h"

namespace {

/** TYPE hnsw. */
class Vec_hnsw_index final : public Vector_index {
 public:
  void open(dict_table_t *table, uint16_t field_no, uint32_t dims, int M,
            int ef_construction) const override {
    (void)vec_open(table, field_no, dims, M, ef_construction);
  }

  [[nodiscard]] dberr_t load(dict_table_t *table, THD *thd) const override {
    return vec_load(table, thd);
  }

  [[nodiscard]] dberr_t insert(trx_t *trx, dict_table_t *table, THD *thd,
                               uint64_t label, const float *vec_data,
                               const byte *row_ref,
                               ulint row_ref_len) const override {
    return vec_insert_point(trx, table, thd, label, vec_data, row_ref,
                            row_ref_len);
  }

  [[nodiscard]] dberr_t remove(trx_t *trx, dict_table_t *table, THD *thd,
                               uint64_t label) const override {
    return vec_delete_point(trx, table, thd, label);
  }

  [[nodiscard]] dberr_t refresh_row_ref(trx_t *trx, dict_table_t *table,
                                        THD *thd, uint64_t label,
                                        const byte *row_ref,
                                        ulint row_ref_len) const override {
    return vec_refresh_row_ref(trx, table, thd, label, row_ref, row_ref_len);
  }

  [[nodiscard]] dberr_t knn(
      dict_table_t *table, THD *thd, const float *query, uint32_t dims,
      size_t k, size_t ef, std::vector<vec_knn_hit_t> *hits,
      const std::unordered_set<uint64_t> *exclude) const override {
    return vec_knn_search(table, thd, query, dims, k, ef, hits, exclude);
  }

  [[nodiscard]] size_t size_hint(const dict_table_t *table) const override {
    return vec_graph_size(table);
  }

  [[nodiscard]] dberr_t build(trx_t *trx, dict_table_t *table,
                              const dict_index_t *vec_index, uint32_t dims,
                              int M, int ef_construction,
                              THD *thd) const override {
    return vec_build_index(trx, table, vec_index, dims, M, ef_construction,
                           thd);
  }

  void close(dict_table_t *table) const override { vec_close(table); }

  [[nodiscard]] dberr_t recreate_after_import(dict_table_t *table,
                                              trx_t *trx) const override {
    return vec_aux_recreate_after_import(table, trx);
  }
};

const Vec_hnsw_index vec_hnsw_singleton;

}  // namespace

const Vector_index *vec_index_for(const dict_table_t *table [[maybe_unused]]) {
  /* R1: hnsw is the only registered TYPE. R2 replaces this with a
  registry lookup keyed by the index's TYPE token; until then every
  vector index in existence is hnsw, and calling any method on a table
  WITHOUT a vector runtime is as safe as the free functions were. */
  return &vec_hnsw_singleton;
}
