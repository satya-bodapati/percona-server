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
The HNSW implementation of the Vector_index seam:
a stateless forwarding shim over the vec0aux free functions. All
per-index state stays in dict_table_t::vec; rollback tracking stays
INSIDE vec_insert_point / vec_delete_point — an implementation detail
this seam deliberately does not expose (spann needs none). */

#include "vec0index.h"

#include "m_string.h"

namespace {

/** TYPE hnsw. */
class Vec_hnsw_index final : public Vector_index {
 public:
  [[nodiscard]] Vec_index_type type() const override {
    return Vec_index_type::HNSW;
  }

  void open(dict_table_t *table, uint16_t field_no, uint32_t dims, int M,
            int ef_construction) const override {
    (void)vec_open(table, this, field_no, dims, M, ef_construction);
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

/** The TYPE registry, indexed by Vec_index_type. Adding an
index TYPE = one enum value + one row here, e.g.
{"spann", &vec_spann_singleton} for a future TYPE. */
struct Vec_type_entry {
  const char *token;
  const Vector_index *impl;
};

const Vec_type_entry vec_type_registry[] = {
    /* order must match Vec_index_type */
    {"hnsw", &vec_hnsw_singleton},
};

}  // namespace

const Vector_index *vec_index_by_name(const char *token, size_t len) {
  if (token == nullptr || len == 0) {
    return nullptr;
  }
  for (const Vec_type_entry &e : vec_type_registry) {
    if (strlen(e.token) == len &&
        native_strncasecmp(e.token, token, len) == 0) {
      return e.impl;
    }
  }
  return nullptr;
}

const Vector_index *vec_index_by_enum(Vec_index_type type) {
  const auto i = static_cast<size_t>(type);
  ut_a(i < UT_ARR_SIZE(vec_type_registry));
  const Vector_index *impl = vec_type_registry[i].impl;
  ut_ad(impl->type() == type);
  return impl;
}

const char *vec_index_token(Vec_index_type type) {
  const auto i = static_cast<size_t>(type);
  ut_a(i < UT_ARR_SIZE(vec_type_registry));
  return vec_type_registry[i].token;
}

const Vector_index *vec_index_for(const dict_table_t *table) {
  /* An open runtime is self-describing: dispatch on the
  implementation that allocated it — this is what makes teardown
  (dict_mem_table_free) correct once several TYPEs coexist, without
  re-resolving the TYPE from the DD. */
  if (table != nullptr && table->vec != nullptr &&
      table->vec->impl != nullptr) {
    return table->vec->impl;
  }

  /* No runtime open. The token-carrying paths (open, build) resolve
  via vec_index_by_name() and never reach here; what does reach here
  is teardown/close on runtime-less tables (a no-op for every type)
  and IMPORT re-mint before first open. hnsw is the correct answer for
  all of them while it is the sole registered type; S1 threads the
  TYPE token through the IMPORT site when the second type lands. */
  return vec_index_by_enum(Vec_index_type::HNSW);
}
