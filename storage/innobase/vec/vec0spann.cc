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
TYPE spann: the build pass (S2) and the runtime + write path (S3).

The static-heads SPANN layout: a random sample of the indexed vectors
becomes the head set (durable as _meta HEAD rows), an in-RAM hnswlib
graph over the heads answers "which lists" queries, and every vector
lands in its nearest head's posting list plus closure copies chosen by
the RNG rule (vec0spann.h). Postings, _meta and (S4) _dead rows are
ordinary transactional rows on whichever trx wrote them — no private
durability, no vec_trx_ops tracking; rollback is pure undo.

The BUILD's head graph is private and discarded at the end; the
RUNTIME (Vec_spann_runtime, below) rebuilds the same graph from the
_meta HEAD range at table open and routes every write_row through it.
A never-built index has no HEAD rows: the implicit genesis head
(VEC_SPANN_GENESIS_HEAD_ID) keeps the head set non-empty. */

#include "vec0spann.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

#include "extra/hnswlib/hnswlib.h"

#include "dict0dict.h"
#include "my_dbug.h"
#include "sync0rw.h"
#include "sync0sync.h"
#include "trx0trx.h"
#include "ut0new.h"
#include "ut0rnd.h"
#include "vec0aux.h"
#include "vec0dml.h"
#include "vec0index.h"

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

/* ------------------------------------------------------------------
The spann runtime (SPANN S3) — the vec_t analog.

Much smaller than hnsw's: the RAM state is a ROUTING structure over
the heads only (10-20% of one table's vectors at most, static between
builds until LIRE), not the index itself. Postings live on disk and
are never mirrored in RAM; there is consequently no memory-budget
integration, no stale/reload fallback (nothing here can half-fail: the
graph is read-only after load), and no per-trx rollback tracking (the
insert path writes rows only — undo is the whole story). */

namespace {

struct Vec_spann_runtime : public Vec_runtime {
  /** head-graph HNSW construction parameters (parser defaults — spann
  has no WITH() surface) */
  int M{0};
  int ef_construction{0};
  /** head graph built from the _meta HEAD range */
  bool loaded{false};
  /** S: routing lookups (insert path); X: (re)load and teardown. The
  head graph is READ-ONLY between loads — heads change only via a
  build (new aux set) until LIRE — so inserts never mutate it. */
  rw_lock_t latch;
  hnswlib::L2Space *space{nullptr};
  hnswlib::HierarchicalNSW<float> *heads{nullptr};
  /** head label -> vector, for the RNG rule's head-to-head distances
  (and S5's re-ranking); owns the storage the graph points into is
  copied from */
  std::unordered_map<uint64_t, std::vector<float>> head_vecs;
  /** per-list appends since load — the LIRE trigger feed (L1);
  observable via spann_dump, otherwise unused in S3. Own mutex, not
  the latch: concurrent inserters all hold the latch in S. std::map
  for deterministic dump order. */
  std::mutex counter_mutex;
  std::map<uint64_t, uint64_t> list_appends;
  /** dead-label inserts since load — the LIRE dead-ratio feed (L2).
  Advisory like list_appends (bumped per statement, not rewound by
  rollback). NOT a dead-label SET: a RAM set has no correct consumer —
  S5's per-reader delete visibility is the read-view scan of _dead
  itself, and a non-transactional set would claim rolled-back deletes.
  If benchmarks show the _dead scan hurting, the fix is a
  committed-only cache keyed by a low-water trx id (S7+), not a set
  maintained from the write path. */
  std::atomic<uint64_t> dead_appends{0};
};

/** The spann runtime of `table`, or nullptr. This file allocated it
(vec_spann_open), so this file alone may interpret the subtype
(SPANN R2). */
inline Vec_spann_runtime *spann_rt(const dict_table_t *table) {
  return static_cast<Vec_spann_runtime *>(table->vec);
}

}  // namespace

void vec_spann_open(dict_table_t *table, const Vector_index *impl,
                    uint16_t field_no, uint32_t dims, int M,
                    int ef_construction) {
  ut_a(table != nullptr);
  ut_a(impl != nullptr);

  if (table->vec != nullptr) {
    return;
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

    auto *rt = ut::new_withkey<Vec_spann_runtime>(
        ut::make_psi_memory_key(mem_key_other));
    rt->table = table;
    rt->impl = impl;
    rt->index_id = vec_index->id;
    rt->field_no = field_no;
    rt->dims = dims;
    rt->M = M;
    rt->ef_construction = ef_construction;
    rw_lock_create(vec_index_rw_lock_key, &rt->latch, LATCH_ID_VEC_INDEX);
    table->vec = rt;
  }
  dict_sys_mutex_exit();
}

/** Build the head graph from _meta. Caller holds rt->latch in X. */
static dberr_t vec_spann_load_locked(Vec_spann_runtime *rt, THD *thd) {
  ut_ad(rw_lock_own(&rt->latch, RW_LOCK_X));

  dict_table_t *table = rt->table;

  MDL_ticket *mdl = nullptr;
  dict_table_t *meta = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "_meta", thd, &mdl);
  if (meta == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  std::vector<vec_spann_head_t> head_rows;
  dberr_t err = vec_spann_meta_load_heads(meta, rt->dims, &head_rows);
  vec_aux_close_for_dml(meta, thd, &mdl);
  if (err != DB_SUCCESS) {
    return err;
  }

  /* No HEAD rows: the implicit genesis head (see
  VEC_SPANN_GENESIS_HEAD_ID) — the head set is never empty. */
  if (head_rows.empty()) {
    head_rows.emplace_back(VEC_SPANN_GENESIS_HEAD_ID,
                           std::vector<float>(rt->dims, 0.0f));
  }

  /* Replace any previous graph (reload after build/DDL). */
  delete rt->heads;
  rt->heads = nullptr;
  delete rt->space;
  rt->space = nullptr;
  rt->head_vecs.clear();

  try {
    rt->space = new hnswlib::L2Space(rt->dims);
    rt->heads = new hnswlib::HierarchicalNSW<float>(rt->space, head_rows.size(),
                                                    rt->M, rt->ef_construction);
    for (const auto &h : head_rows) {
      rt->heads->addPoint(h.second.data(), h.first, false, nullptr);
    }
  } catch (const std::exception &e) {
    ib::warn() << "vec_spann_load: head graph construction failed for "
               << table->name.m_name << " (dims=" << rt->dims
               << " heads=" << head_rows.size() << "): " << e.what();
    delete rt->heads;
    rt->heads = nullptr;
    delete rt->space;
    rt->space = nullptr;
    return DB_OUT_OF_MEMORY;
  }

  rt->head_vecs.reserve(head_rows.size());
  for (auto &h : head_rows) {
    rt->head_vecs.emplace(h.first, std::move(h.second));
  }

  {
    std::lock_guard<std::mutex> g(rt->counter_mutex);
    rt->list_appends.clear();
  }

  rt->loaded = true;

  return DB_SUCCESS;
}

dberr_t vec_spann_load(dict_table_t *table, THD *thd) {
  Vec_spann_runtime *rt = spann_rt(table);
  ut_a(rt != nullptr);

  rw_lock_x_lock(&rt->latch, UT_LOCATION_HERE);
  dberr_t err = DB_SUCCESS;
  if (!rt->loaded) {
    err = vec_spann_load_locked(rt, thd);
  }
  rw_lock_x_unlock(&rt->latch);
  return err;
}

dberr_t vec_spann_insert_point(trx_t *trx, dict_table_t *table, THD *thd,
                               uint64_t label, const float *vec_data,
                               const byte *row_ref, ulint row_ref_len) {
  Vec_spann_runtime *rt = spann_rt(table);
  ut_a(rt != nullptr);
  ut_a(vec_data != nullptr);
  ut_a(row_ref != nullptr);

  MDL_ticket *mdl = nullptr;
  dict_table_t *postings = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "", thd, &mdl);
  if (postings == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  if (!rt->loaded) {
    rw_lock_s_unlock(&rt->latch);
    dberr_t lerr = vec_spann_load(table, thd);
    if (lerr != DB_SUCCESS) {
      vec_aux_close_for_dml(postings, thd, &mdl);
      return lerr;
    }
    rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  }

  dberr_t err = DB_SUCCESS;
  std::vector<uint64_t> selected;

  {
    const size_t k =
        std::min<size_t>(VEC_SPANN_CLOSURE_MAX, rt->head_vecs.size());
    const hnswlib::DISTFUNC<float> dist_func = rt->space->get_dist_func();
    void *dist_param = rt->space->get_dist_func_param();

    std::vector<vec_spann_head_cand_t> cands;
    try {
      const auto found = rt->heads->searchKnnCloserFirst(vec_data, k);
      cands.reserve(found.size());
      for (const auto &c : found) {
        cands.push_back({c.first, static_cast<uint64_t>(c.second)});
      }
    } catch (...) {
      err = DB_OUT_OF_MEMORY;
    }

    if (err == DB_SUCCESS) {
      vec_spann_select_heads(
          cands, VEC_SPANN_CLOSURE_EPS, VEC_SPANN_CLOSURE_MAX,
          [&](uint64_t a, uint64_t b) {
            return dist_func(rt->head_vecs.at(a).data(),
                             rt->head_vecs.at(b).data(), dist_param);
          },
          &selected);
      ut_a(!selected.empty()); /* the head set is never empty */

      /* Posting appends on the USER trx, still under the S latch (the
      hnsw insert path holds it across its aux DML too): the only X
      taker while a table is open for DML is a reload, which must not
      swap the head set out from under a half-routed insert. */
      for (const uint64_t head_id : selected) {
        err = vec_spann_posting_insert(trx, postings, head_id, label, vec_data,
                                       rt->dims, row_ref, row_ref_len);
        if (err != DB_SUCCESS) {
          break;
        }
      }
    }
  }

  if (err == DB_SUCCESS) {
    std::lock_guard<std::mutex> g(rt->counter_mutex);
    for (const uint64_t head_id : selected) {
      ++rt->list_appends[head_id];
    }
  }

  rw_lock_s_unlock(&rt->latch);
  vec_aux_close_for_dml(postings, thd, &mdl);

  return err;
}

dberr_t vec_spann_remove_point(trx_t *trx, dict_table_t *table, THD *thd,
                               uint64_t label) {
  Vec_spann_runtime *rt = spann_rt(table);
  ut_a(rt != nullptr);

  /* No latch, no head graph, no load: retiring a label is one row in
  _dead on the user trx, whatever the routing state is. */
  MDL_ticket *mdl = nullptr;
  dict_table_t *dead = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "_dead", thd, &mdl);
  if (dead == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  const dberr_t err = vec_spann_dead_insert(trx, dead, label);

  if (err == DB_SUCCESS) {
    rt->dead_appends.fetch_add(1, std::memory_order_relaxed);
  }

  vec_aux_close_for_dml(dead, thd, &mdl);
  return err;
}

void vec_spann_close(dict_table_t *table) {
  Vec_spann_runtime *rt = spann_rt(table);
  if (rt == nullptr) {
    return;
  }
  table->vec = nullptr;

  delete rt->heads;
  delete rt->space;
  /* The rw_lock_t member destructor tears the latch down when the
  runtime is deleted as a C++ object — same rule as vec_close. */
  ut::delete_(rt);
}

bool vec_spann_runtime_stats(dict_table_t *table, vec_spann_stats_t *stats) {
  ut_a(stats != nullptr);

  /* Type check through the impl back-pointer (SPANN R2): only then is
  the downcast below legal. */
  if (table == nullptr || table->vec == nullptr ||
      table->vec->impl == nullptr ||
      table->vec->impl->type() != Vec_index_type::SPANN) {
    return false;
  }
  Vec_spann_runtime *rt = spann_rt(table);

  rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  if (!rt->loaded) {
    rw_lock_s_unlock(&rt->latch);
    return false;
  }
  stats->n_heads = rt->head_vecs.size();
  stats->dead_appends = rt->dead_appends.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> g(rt->counter_mutex);
    stats->list_appends.assign(rt->list_appends.begin(),
                               rt->list_appends.end());
  }
  rw_lock_s_unlock(&rt->latch);
  return true;
}
