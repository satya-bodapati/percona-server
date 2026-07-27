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

/** TYPE spann — S1: aux lifecycle only. open() is deliberately a no-op:
table->vec stays nullptr, so every DML hook skips (the index exists but
is not populated — the phase-1 posture), and the optimizer's JT_VECTOR
gate is restricted to hnsw (sql_optimizer.cc), so queries run the exact
path with correct results. The write path lands in S3, the read path in
S5; the runtime-op stubs below are unreachable until then and say so. */
class Vec_spann_index final : public Vector_index {
 public:
  [[nodiscard]] Vec_index_type type() const override {
    return Vec_index_type::SPANN;
  }

  void open(dict_table_t *, uint16_t, uint32_t, int, int) const override {
    /* S3 creates Vec_spann_runtime here. */
  }

  [[nodiscard]] dberr_t load(dict_table_t *, THD *) const override {
    return DB_SUCCESS; /* nothing to load until S5/S6 */
  }

  [[nodiscard]] dberr_t insert(trx_t *, dict_table_t *, THD *, uint64_t,
                               const float *, const byte *,
                               ulint) const override {
    ut_d(ut_error); /* unreachable: no runtime => hooks skip (S3) */
    ut_o(return DB_UNSUPPORTED);
  }

  [[nodiscard]] dberr_t remove(trx_t *, dict_table_t *, THD *,
                               uint64_t) const override {
    ut_d(ut_error); /* unreachable until S4 */
    ut_o(return DB_UNSUPPORTED);
  }

  [[nodiscard]] dberr_t refresh_row_ref(trx_t *, dict_table_t *, THD *,
                                        uint64_t, const byte *,
                                        ulint) const override {
    ut_d(ut_error); /* unreachable until S4 */
    ut_o(return DB_UNSUPPORTED);
  }

  [[nodiscard]] dberr_t knn(
      dict_table_t *, THD *, const float *, uint32_t, size_t, size_t,
      std::vector<vec_knn_hit_t> *,
      const std::unordered_set<uint64_t> *) const override {
    ut_d(ut_error); /* unreachable: optimizer gate is hnsw-only until S5 */
    ut_o(return DB_UNSUPPORTED);
  }

  [[nodiscard]] size_t size_hint(const dict_table_t *) const override {
    return 0;
  }

  [[nodiscard]] dberr_t build(trx_t *, dict_table_t *, const dict_index_t *,
                              uint32_t, int, int, THD *) const override {
    return DB_SUCCESS; /* S2: head selection + assignment; empty until then */
  }

  void close(dict_table_t *) const override { /* no runtime yet */
  }

  [[nodiscard]] dberr_t recreate_after_import(dict_table_t *table,
                                              trx_t *trx) const override {
    /* Generic: re-mints THIS index's aux set (the create path is
    table-set-driven by dict_index_t::vec_type) + re-seeds the counter. */
    return vec_aux_recreate_after_import(table, trx);
  }
};

const Vec_spann_index vec_spann_singleton;

/** The TYPE registry (SPANN R3), indexed by Vec_index_type. Adding an
index TYPE = one enum value + one row here (S1 adds
{"spann", &vec_spann_singleton}). */
struct Vec_type_entry {
  const char *token;
  const Vector_index *impl;
};

const Vec_type_entry vec_type_registry[] = {
    /* order must match Vec_index_type */
    {"hnsw", &vec_hnsw_singleton},
    {"spann", &vec_spann_singleton},
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
  /* An open runtime is self-describing (SPANN R2): dispatch on the
  implementation that allocated it — this is what makes teardown
  (dict_mem_table_free) correct once several TYPEs coexist, without
  re-resolving the TYPE from the DD. */
  if (table != nullptr && table->vec != nullptr &&
      table->vec->impl != nullptr) {
    return table->vec->impl;
  }

  /* No runtime open: resolve from the index's registered type —
  dict_index_t::vec_type, set at creation and DD reload (S1). Covers
  the runtime-less dispatch sites: teardown/close and IMPORT re-mint
  before first open. */
  if (table != nullptr) {
    for (const dict_index_t *idx = table->first_index(); idx != nullptr;
         idx = idx->next()) {
      if (idx->is_vector()) {
        return vec_index_by_enum(idx->vec_type);
      }
    }
  }

  /* No vector index at all: any implementation's close() is a no-op;
  keep the historical answer. */
  return vec_index_by_enum(Vec_index_type::HNSW);
}
