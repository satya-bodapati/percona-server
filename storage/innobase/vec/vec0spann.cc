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

/** @file vec/vec0spann.cc
TYPE spann build pass (SPANN plan, S2).

The static-heads SPANN layout: a random sample of the indexed vectors
becomes the head set (durable as _meta HEAD rows), an in-RAM hnswlib
graph over the heads answers "which lists" queries, and every vector
lands in its nearest head's posting list plus closure copies chosen by
the RNG rule (vec0spann.h). The postings and _meta rows are ordinary
transactional rows on the ALTER's trx — no private durability, no
tracking (contrast vec0aux.cc's hnsw build, whose graph mutations need
vec_trx_ops); rollback is pure undo.

The head graph built here is PRIVATE and discarded at the end: S2 has
no runtime (table->vec stays nullptr), so write hooks and reads still
skip. S3/S5 hang a Vec_spann_runtime with a graph rebuilt from the
_meta HEAD range off the same seam. */

#include "vec0spann.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

#include "extra/hnswlib/hnswlib.h"

#include "dict0dict.h"
#include "my_dbug.h"
#include "trx0trx.h"
#include "ut0rnd.h"
#include "vec0aux.h"
#include "vec0dml.h"

dberr_t vec_spann_build_index(trx_t *trx, dict_table_t *table,
                              const dict_index_t *vec_index, uint32_t dims,
                              int M, int ef_construction, THD *thd) {
  ut_a(trx != nullptr);
  ut_a(vec_index != nullptr);
  ut_a(dims != 0);

  std::vector<vec_base_row_t> rows;
  dberr_t err = vec_base_collect_rows(trx, table, vec_index, dims, &rows);
  if (err != DB_SUCCESS) {
    return err;
  }
  if (rows.empty()) {
    return DB_SUCCESS; /* empty index: no heads, no postings */
  }

  /* Head sample: HEADS_PCT of the rows, floor 1. Uniform-random —
  sampled heads represent the data distribution without any training
  pass (the SPANN premise); LIRE (L-phase) repairs skew later. The
  fixed-seed DBUG knob makes the whole build — sample, graph, closure —
  deterministic for MTR. */
  const size_t n_heads =
      std::max<size_t>(1, rows.size() * VEC_SPANN_HEADS_PCT / 100);

  uint64_t seed = ut::random_64();
  DBUG_EXECUTE_IF("spann_build_seed_42", seed = 42;);

  std::vector<size_t> order(rows.size());
  std::iota(order.begin(), order.end(), 0);
  {
    std::mt19937_64 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
  }
  order.resize(n_heads);
  /* Ascending-label head order: _meta insert order matches the (HEAD,
  id) PK — right-edge appends — and keeps head identity independent of
  the shuffle's suffix. */
  std::sort(order.begin(), order.end(), [&rows](size_t a, size_t b) {
    return std::get<0>(rows[a]) < std::get<0>(rows[b]);
  });

  MDL_ticket *post_mdl = nullptr;
  dict_table_t *postings = vec_aux_open_for_dml(
      table, vec_index->id, Vec_index_type::SPANN, "", thd, &post_mdl);
  if (postings == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }
  MDL_ticket *meta_mdl = nullptr;
  dict_table_t *meta = vec_aux_open_for_dml(
      table, vec_index->id, Vec_index_type::SPANN, "_meta", thd, &meta_mdl);
  if (meta == nullptr) {
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_TABLE_NOT_FOUND;
  }

  hnswlib::L2Space *space = nullptr;
  hnswlib::HierarchicalNSW<float> *head_graph = nullptr;

  try {
    space = new hnswlib::L2Space(dims);
    head_graph =
        new hnswlib::HierarchicalNSW<float>(space, n_heads, M, ef_construction);
  } catch (...) {
    delete head_graph;
    delete space;
    vec_aux_close_for_dml(meta, thd, &meta_mdl);
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_OUT_OF_MEMORY;
  }

  /* label -> head vector, for the RNG rule's head-to-head distances.
  Pointers into `rows`, which outlives every use. */
  std::unordered_map<uint64_t, const float *> head_vecs;
  head_vecs.reserve(n_heads);

  const hnswlib::DISTFUNC<float> dist_func = space->get_dist_func();
  void *dist_param = space->get_dist_func_param();

  /* Heads: durable identity first (_meta HEAD rows on the ALTER trx),
  then the private RAM graph. No persistence callbacks — the posting
  rows below are the durable assignment, the graph is scaffolding. */
  for (const size_t pos : order) {
    const uint64_t label = std::get<0>(rows[pos]);
    const std::vector<float> &v = std::get<1>(rows[pos]);

    err = vec_spann_meta_insert(trx, meta, VEC_SPANN_META_HEAD, label,
                                reinterpret_cast<const byte *>(v.data()),
                                dims * sizeof(float));
    if (err != DB_SUCCESS) {
      break;
    }

    try {
      head_graph->addPoint(v.data(), label, false, nullptr);
    } catch (...) {
      err = DB_OUT_OF_MEMORY;
      break;
    }
    head_vecs.emplace(label, v.data());
  }

  /* Assignment: nearest head + closure copies for every row, heads
  included (a head's own vector is a posting in its own list — the
  head row in _meta is identity, not storage). */
  if (err == DB_SUCCESS) {
    const size_t k = std::min<size_t>(VEC_SPANN_CLOSURE_MAX, n_heads);
    std::vector<vec_spann_head_cand_t> cands;
    std::vector<uint64_t> selected;

    for (const auto &row : rows) {
      const uint64_t label = std::get<0>(row);
      const std::vector<float> &v = std::get<1>(row);

      cands.clear();
      try {
        const auto found = head_graph->searchKnnCloserFirst(v.data(), k);
        cands.reserve(found.size());
        for (const auto &c : found) {
          cands.push_back({c.first, static_cast<uint64_t>(c.second)});
        }
      } catch (...) {
        err = DB_OUT_OF_MEMORY;
        break;
      }

      vec_spann_select_heads(
          cands, VEC_SPANN_CLOSURE_EPS, VEC_SPANN_CLOSURE_MAX,
          [&](uint64_t a, uint64_t b) {
            return dist_func(head_vecs.at(a), head_vecs.at(b), dist_param);
          },
          &selected);
      /* n_heads >= 1 and the search width is >= 1: never unassigned. */
      ut_a(!selected.empty());

      for (const uint64_t head_id : selected) {
        err = vec_spann_posting_insert(trx, postings, head_id, label, v.data(),
                                       dims, std::get<2>(row).data(),
                                       std::get<2>(row).size());
        if (err != DB_SUCCESS) {
          break;
        }
      }
      if (err != DB_SUCCESS) {
        break;
      }
    }
  }

  delete head_graph;
  delete space;
  vec_aux_close_for_dml(meta, thd, &meta_mdl);
  vec_aux_close_for_dml(postings, thd, &post_mdl);

  return err;
}
