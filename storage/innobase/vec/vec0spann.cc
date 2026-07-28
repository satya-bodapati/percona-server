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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "extra/hnswlib/hnswlib.h"

#include "current_thd.h"
#include "debug_sync.h"
#include "dict0dict.h"
#include "ha_innodb.h" /* thd_to_trx */
#include "my_dbug.h"
#include "read0read.h"
#include "srv0mon.h"
#include "sync0rw.h"
#include "sync0sync.h"
#include "trx0roll.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "ut0new.h"
#include "ut0rnd.h"
#include "vec0aux.h"
#include "vec0dml.h"
#include "vec0index.h"
#include "vec0maint.h"

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

  /** Retired heads (phase L): head -> the maintenance trx that
  retired it. THE TRANSITION PROTOCOL IN ONE FIELD — a retired head
  stays in the RAM graph so readers keep probing its list (their read
  views decide what they see there; a pre-swap view finds the old
  world, a post-swap view finds the list superseded), but the WRITE
  routing filters it out, so its list is append-frozen the moment it
  retires. Once no active read view predates the retirement
  (clone_oldest_view check), the head is dropped from the graph
  (markDelete) — its list is then unreachable garbage for L2 to trim.
  Modified under the latch in X; read under S. */
  std::map<uint64_t, trx_id_t> retired;
};

/** Write-routing filter: retired heads take no new postings. */
class Vec_live_head_filter : public hnswlib::BaseFilterFunctor {
 public:
  explicit Vec_live_head_filter(const std::map<uint64_t, trx_id_t> &retired)
      : m_retired(retired) {}
  bool operator()(hnswlib::labeltype label) override {
    return m_retired.count(label) == 0;
  }

 private:
  const std::map<uint64_t, trx_id_t> &m_retired;
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

  /* Replace any previous graph (reload after build/DDL). Retirements
  do not survive a reload: _meta no longer has the retired heads, and
  a reload can only run where no reader is mid-flight (fresh open). */
  delete rt->heads;
  rt->heads = nullptr;
  delete rt->space;
  rt->space = nullptr;
  rt->head_vecs.clear();
  rt->retired.clear();

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
    /* Route against LIVE heads only: a retired head's list is
    append-frozen from the moment of retirement (see
    Vec_spann_runtime::retired), so a row landing there could be lost
    to post-GC readers. */
    const size_t n_live = rt->head_vecs.size() - rt->retired.size();
    ut_a(n_live > 0); /* the head set is never empty */
    const size_t k = std::min<size_t>(VEC_SPANN_CLOSURE_MAX, n_live);
    const hnswlib::DISTFUNC<float> dist_func = rt->space->get_dist_func();
    void *dist_param = rt->space->get_dist_func_param();

    std::vector<vec_spann_head_cand_t> cands;
    try {
      Vec_live_head_filter filter(rt->retired);
      const auto found = rt->heads->searchKnnCloserFirst(
          vec_data, k, rt->retired.empty() ? nullptr : &filter);
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

  std::vector<uint64_t> to_split;
  if (err == DB_SUCCESS) {
    /* The L1 split trigger: crossing a threshold multiple enqueues a
    split of that list (multiples, not one edge, so an aborted or
    no-op split re-arms as the list keeps growing). */
    uint64_t threshold = VEC_SPANN_SPLIT_THRESHOLD;
    DBUG_EXECUTE_IF("spann_split_threshold_low", threshold = 8;);
    std::lock_guard<std::mutex> g(rt->counter_mutex);
    for (const uint64_t head_id : selected) {
      if (++rt->list_appends[head_id] % threshold == 0) {
        to_split.push_back(head_id);
      }
    }
  }

  rw_lock_s_unlock(&rt->latch);
  vec_aux_close_for_dml(postings, thd, &mdl);

  for (const uint64_t head_id : to_split) {
    vec_maint_enqueue(Vec_maint_op::SPLIT, table->id, head_id, nullptr,
                      nullptr);
  }

  return err;
}

/* ------------------------------------------------------------------
Phase L: the head-set transition protocol and its jobs.

The one idea: readers never coordinate with maintenance. The RAM graph
holds the UNION of live and retired heads; every reader probes it and
its own read view decides, list by list, whether it sees the old world
or the new one; label dedup absorbs rows found in both. Writers route
around retired heads, so retired lists are append-frozen and can be
garbage-collected once every active view postdates the retirement. */

/** Add `new_heads` to the live graph and retire `old_heads` as of
`retire_id` (the maintenance trx). Caller holds rt->latch in X and has
COMMITTED the transaction that made the corresponding _meta swap
durable — publish only re-states committed truth in RAM. */
static void vec_spann_publish_locked(
    Vec_spann_runtime *rt, const std::vector<vec_spann_head_t> &new_heads,
    const std::vector<uint64_t> &old_heads, trx_id_t retire_id) {
  ut_ad(rw_lock_own(&rt->latch, RW_LOCK_X));

  /* Grow the graph if needed (resizeIndex is X-latch-only, which we
  hold). */
  const size_t need = rt->heads->cur_element_count.load() + new_heads.size();
  if (need > rt->heads->max_elements_) {
    rt->heads->resizeIndex(std::max(need, 2 * rt->heads->max_elements_));
  }

  for (const auto &h : new_heads) {
    rt->heads->addPoint(h.second.data(), h.first, false, nullptr);
    rt->head_vecs.emplace(h.first, h.second);
  }
  for (const uint64_t h : old_heads) {
    rt->retired.emplace(h, retire_id);
  }

  /* New generation, new split-trigger baseline. */
  {
    std::lock_guard<std::mutex> g(rt->counter_mutex);
    rt->list_appends.clear();
  }
}

/** Drop retired heads no active read view can still need: once the
oldest view sees `retire_id` as committed, every reader also sees the
new generation's postings, so the retired list is pure garbage (L2
trims the rows). markDelete hides the head from future probes. */
static void vec_spann_gc_retired(dict_table_t *table) {
  Vec_spann_runtime *rt = spann_rt(table);
  if (rt == nullptr) {
    return;
  }

  rw_lock_x_lock(&rt->latch, UT_LOCATION_HERE);
  if (rt->loaded && !rt->retired.empty()) {
    ReadView oldest;
    trx_sys->mvcc->clone_oldest_view(&oldest);

    for (auto it = rt->retired.begin(); it != rt->retired.end();) {
      if (oldest.changes_visible(it->second, table->name)) {
        try {
          rt->heads->markDelete(it->first);
        } catch (...) {
          /* not in the graph: nothing to hide */
        }
        rt->head_vecs.erase(it->first);
        it = rt->retired.erase(it);
      } else {
        ++it;
      }
    }
  }
  rw_lock_x_unlock(&rt->latch);
}

/** One collected (label, vector, row_ref) for regeneration. */
struct vec_spann_snap_row_t {
  uint64_t label;
  std::vector<float> vec;
  std::string ref;
};

/** Load the committed dead-label set on a SCOPED read-only
transaction: the view opens, the (small) _dead table is scanned, the
trx commits and the view closes — milliseconds. Maintenance jobs use
this instead of assigning a view to their own long transaction, so no
job ever pins purge's low-water mark beyond one _dead scan (a long
registered view stalls purge exactly like any long reader would). */
static dberr_t vec_spann_dead_set_scoped(dict_table_t *dead,
                                         std::unordered_set<uint64_t> *out) {
  trx_t *trx = trx_allocate_for_background();
  trx_start_internal_read_only(trx, UT_LOCATION_HERE);
  ReadView *view = trx_assign_read_view(trx);
  const dberr_t err = view == nullptr
                          ? DB_OUT_OF_RESOURCES
                          : vec_spann_load_dead_set(dead, view, out);
  trx_commit_for_mysql(trx);
  trx_free_for_background(trx);
  return err;
}

dberr_t vec_spann_resample(dict_table_t *table, THD *thd) {
  Vec_spann_runtime *rt = spann_rt(table);
  if (rt == nullptr) {
    /* Not opened since restart: nobody is writing through it either;
    nothing to rebalance that a later open + enqueue cannot redo. */
    return DB_TABLE_NOT_FOUND;
  }

  rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  if (!rt->loaded) {
    rw_lock_s_unlock(&rt->latch);
    dberr_t lerr = vec_spann_load(table, thd);
    if (lerr != DB_SUCCESS) {
      return lerr;
    }
    rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  }
  /* The OLD generation = the currently live heads. */
  std::vector<uint64_t> old_live;
  old_live.reserve(rt->head_vecs.size());
  for (const auto &hv : rt->head_vecs) {
    if (rt->retired.count(hv.first) == 0) {
      old_live.push_back(hv.first);
    }
  }
  const uint32_t dims = rt->dims;
  rw_lock_s_unlock(&rt->latch);

  /* Labels above this are the catch-up suffix: everything the
  snapshot below can miss was stamped after this point and routed to
  the old live heads (routing flips only at publish). */
  const uint64_t snap_next = table->vec_next_id.load();

  MDL_ticket *post_mdl = nullptr;
  dict_table_t *postings = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "", thd, &post_mdl);
  if (postings == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }
  MDL_ticket *meta_mdl = nullptr;
  dict_table_t *meta = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "_meta", thd, &meta_mdl);
  MDL_ticket *dead_mdl = nullptr;
  dict_table_t *dead =
      meta == nullptr
          ? nullptr
          : vec_aux_open_for_dml(table, rt->index_id, Vec_index_type::SPANN,
                                 "_dead", thd, &dead_mdl);
  if (meta == nullptr || dead == nullptr) {
    if (meta != nullptr) {
      vec_aux_close_for_dml(meta, thd, &meta_mdl);
    }
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_TABLE_NOT_FOUND;
  }

  /* Dead pruning decisions come from a SCOPED committed view (see
  vec_spann_dead_set_scoped) — no transaction of this job carries a
  read view, so a resample of any size cannot stall purge. */
  std::unordered_set<uint64_t> dead_set;
  dberr_t err = vec_spann_dead_set_scoped(dead, &dead_set);

  /* Read-only, viewless companion for the snapshot scans (LOB reads
  want a trx context; latest-read wants no view). */
  trx_t *scan_trx = trx_allocate_for_background();
  trx_start_internal_read_only(scan_trx, UT_LOCATION_HERE);

  /* Snapshot: every label with its vector and row_ref, deduped across
  closure copies, straight from the old posting lists — the base table
  is never touched. LATEST-read on purpose: a label stamped at or
  below snap_next whose inserting trx has not committed yet is
  invisible to any read view AND outside the catch-up suffix — a
  view-based snapshot would lose it (found by the first split test:
  the very insert that triggers a job is still uncommitted when the
  job starts). Uncommitted rows' copies commit with the job; if theirs
  rolls back the copy is a fetch-miss orphan until GC's orphan rule
  trims it. Dead pruning stays VIEW-based: pruning on an uncommitted
  DELETE would lose the row if that DELETE rolled back — an extra copy
  of a dead row is harmless (readers filter per view), a lost live row
  is not. */
  std::vector<vec_spann_snap_row_t> rows;
  {
    std::unordered_set<uint64_t> seen;
    for (const uint64_t h : old_live) {
      if (err != DB_SUCCESS) {
        break;
      }
      if (vec_maint_is_canceled(table->id)) {
        err = DB_INTERRUPTED;
        break;
      }
      err = vec_spann_scan_list(
          scan_trx, postings, h, dims, nullptr /* latest */,
          [&](uint64_t label, const float *vec, const byte *ref,
              ulint ref_len) {
            if (dead_set.count(label) != 0 || !seen.insert(label).second) {
              return;
            }
            vec_spann_snap_row_t r;
            r.label = label;
            r.vec.assign(vec, vec + dims);
            r.ref.assign(reinterpret_cast<const char *>(ref), ref_len);
            rows.push_back(std::move(r));
          });
    }
  }

  if (err == DB_SUCCESS && rows.empty()) {
    /* Nothing to regenerate; keep the old head set (it is never
    replaced by an EMPTY one — the genesis rule needs a head). */
    trx_commit_for_mysql(scan_trx);
    trx_free_for_background(scan_trx);
    vec_aux_close_for_dml(dead, thd, &dead_mdl);
    vec_aux_close_for_dml(meta, thd, &meta_mdl);
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_SUCCESS;
  }

  /* New head set: sampled from the snapshot (same rule as the S2
  build), but with FRESH counter ids — head identity and label
  identity share the crash-safe counter, so a regenerated head can
  never collide with any label or any prior head. The _meta rows are
  written by the SWAP transaction below, not here: until they commit,
  every posting staged under these ids is unreachable garbage at
  worst. */
  std::vector<vec_spann_head_t> new_heads;
  hnswlib::L2Space *space = nullptr;
  hnswlib::HierarchicalNSW<float> *route = nullptr;

  if (err == DB_SUCCESS) {
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
    std::sort(order.begin(), order.end(), [&rows](size_t a, size_t b) {
      return rows[a].label < rows[b].label;
    });

    try {
      space = new hnswlib::L2Space(dims);
      route = new hnswlib::HierarchicalNSW<float>(space, n_heads, rt->M,
                                                  rt->ef_construction);
      for (const size_t pos : order) {
        const uint64_t head_id = vec_assign_next_idx_id(table);
        route->addPoint(rows[pos].vec.data(), head_id, false, nullptr);
        new_heads.emplace_back(head_id, rows[pos].vec);
      }
    } catch (...) {
      err = DB_OUT_OF_MEMORY;
    }
  }

  /* Assignment closure over the new heads (S2's rule verbatim). */
  const hnswlib::DISTFUNC<float> dist_func =
      space != nullptr ? space->get_dist_func() : nullptr;
  void *dist_param = space != nullptr ? space->get_dist_func_param() : nullptr;

  const auto head_dist = [&](uint64_t a, uint64_t b) {
    auto va = std::lower_bound(
        new_heads.begin(), new_heads.end(), a,
        [](const vec_spann_head_t &h, uint64_t id) { return h.first < id; });
    auto vb = std::lower_bound(
        new_heads.begin(), new_heads.end(), b,
        [](const vec_spann_head_t &h, uint64_t id) { return h.first < id; });
    return dist_func(va->second.data(), vb->second.data(), dist_param);
  };

  const auto assign_one = [&](trx_t *wtrx,
                              const vec_spann_snap_row_t &r) -> dberr_t {
    const size_t k = std::min<size_t>(VEC_SPANN_CLOSURE_MAX, new_heads.size());
    std::vector<vec_spann_head_cand_t> cands;
    try {
      const auto found = route->searchKnnCloserFirst(r.vec.data(), k);
      cands.reserve(found.size());
      for (const auto &c : found) {
        cands.push_back({c.first, static_cast<uint64_t>(c.second)});
      }
    } catch (...) {
      return DB_OUT_OF_MEMORY;
    }
    std::vector<uint64_t> selected;
    vec_spann_select_heads(cands, VEC_SPANN_CLOSURE_EPS, VEC_SPANN_CLOSURE_MAX,
                           head_dist, &selected);
    ut_a(!selected.empty());
    for (const uint64_t h : selected) {
      const dberr_t ierr = vec_spann_posting_insert(
          wtrx, postings, h, r.label, r.vec.data(), dims,
          reinterpret_cast<const byte *>(r.ref.data()), r.ref.size());
      if (ierr != DB_SUCCESS) {
        return ierr;
      }
    }
    return DB_SUCCESS;
  };

  /* PHASE A — batched staging. The new generation is UNREACHABLE
  until the swap commits (its head ids exist in neither _meta nor the
  RAM graph), so these inserts may commit in small view-free batches:
  bounded undo, no long transaction, purge never waits. A crash or
  cancellation abandons only unreachable rows, which the existing
  garbage-list GC rule sweeps. */
  if (err == DB_SUCCESS) {
    size_t pos = 0;
    while (err == DB_SUCCESS && pos < rows.size()) {
      trx_t *batch_trx = trx_allocate_for_background();
      trx_start_internal(batch_trx, UT_LOCATION_HERE);

      const size_t batch_end =
          std::min(pos + VEC_SPANN_RESAMPLE_BATCH, rows.size());
      for (; pos < batch_end && err == DB_SUCCESS; ++pos) {
        err = assign_one(batch_trx, rows[pos]);
      }

      if (err == DB_SUCCESS) {
        trx_commit_for_mysql(batch_trx);
      } else {
        trx_rollback_to_savepoint(batch_trx, nullptr);
      }
      trx_free_for_background(batch_trx);

      if (err == DB_SUCCESS && vec_maint_is_canceled(table->id)) {
        err = DB_INTERRUPTED;
      }
    }
  }

  /* Test choreography hook: pause with the new generation fully
  staged (committed but unreachable) and the swap not yet begun —
  concurrent DML runs against the old world here. */
  DBUG_EXECUTE_IF("spann_resample_pause", {
    const char act[] =
        "now SIGNAL spann_resample_paused WAIT_FOR spann_resample_go";
    ut_a(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });

  /* Last pre-swap cancellation point — in particular the one a DDL
  that queued up behind the test pause above relies on. */
  if (err == DB_SUCCESS && vec_maint_is_canceled(table->id)) {
    err = DB_INTERRUPTED;
  }

  /* PHASE B — the atomic swap, deliberately SMALL: new head rows in,
  old head rows out, the concurrent-insert suffix carried, one commit.
  This is the only crash-atomicity that matters; everything bulky
  already sits committed and unreachable. */
  if (err == DB_SUCCESS) {
    trx_t *swap_trx = trx_allocate_for_background();
    trx_start_internal(swap_trx, UT_LOCATION_HERE);

    for (const auto &h : new_heads) {
      err =
          vec_spann_meta_insert(swap_trx, meta, VEC_SPANN_META_HEAD, h.first,
                                reinterpret_cast<const byte *>(h.second.data()),
                                dims * sizeof(float));
      if (err != DB_SUCCESS) {
        break;
      }
    }

    /* Retire the old generation's identity: plain row DELETEs, so
    MVCC serves the old head set to whoever still needs it and purge
    reclaims. The genesis head has no _meta row by definition. */
    if (err == DB_SUCCESS) {
      for (const uint64_t h : old_live) {
        if (h == VEC_SPANN_GENESIS_HEAD_ID) {
          continue;
        }
        const dberr_t derr =
            vec_spann_meta_delete(swap_trx, meta, VEC_SPANN_META_HEAD, h);
        if (derr != DB_SUCCESS && derr != DB_RECORD_NOT_FOUND) {
          err = derr;
          break;
        }
      }
    }

    if (err == DB_SUCCESS) {
      /* THE FENCE. X on the runtime latch: no insert is mid-route
      (they hold S across route+append), so the old lists' suffixes
      are final once scanned here. Latest-read for the same reason as
      the snapshot. */
      rw_lock_x_lock(&rt->latch, UT_LOCATION_HERE);

      /* Collect the suffix first, insert after: DML inside a scan
      callback would run under the scan's open mini-transaction
      (log_free_check forbids exactly that). */
      std::vector<vec_spann_snap_row_t> catchup;
      std::unordered_set<uint64_t> caught;
      for (const uint64_t h : old_live) {
        if (err != DB_SUCCESS) {
          break;
        }
        err = vec_spann_scan_list(
            swap_trx, postings, h, dims, nullptr /* latest */,
            [&](uint64_t label, const float *vec, const byte *ref,
                ulint ref_len) {
              if (!caught.insert(label).second) {
                return;
              }
              vec_spann_snap_row_t r;
              r.label = label;
              r.vec.assign(vec, vec + dims);
              r.ref.assign(reinterpret_cast<const char *>(ref), ref_len);
              catchup.push_back(std::move(r));
            },
            snap_next /* label_gt: the suffix */);
      }
      for (const auto &r : catchup) {
        if (err != DB_SUCCESS) {
          break;
        }
        err = assign_one(swap_trx, r);
      }

      if (err == DB_SUCCESS) {
        /* Commit under the fence (one redo sync), then re-state the
        committed truth in RAM. A crash between the two leaves the
        committed new world on disk and a stale RAM graph that the
        restart reload replaces — never the reverse. */
        const trx_id_t retire_id = swap_trx->id;
        trx_commit_for_mysql(swap_trx);
        vec_spann_publish_locked(rt, new_heads, old_live, retire_id);
        rw_lock_x_unlock(&rt->latch);
      } else {
        rw_lock_x_unlock(&rt->latch);
        trx_rollback_to_savepoint(swap_trx, nullptr);
      }
    } else {
      trx_rollback_to_savepoint(swap_trx, nullptr);
    }
    trx_free_for_background(swap_trx);
  }

  delete route;
  delete space;
  trx_commit_for_mysql(scan_trx);
  trx_free_for_background(scan_trx);
  vec_aux_close_for_dml(dead, thd, &dead_mdl);
  vec_aux_close_for_dml(meta, thd, &meta_mdl);
  vec_aux_close_for_dml(postings, thd, &post_mdl);

  if (err == DB_SUCCESS) {
    MONITOR_INC(MONITOR_VEC_SPANN_RESAMPLES);
    vec_spann_gc_retired(table);
  }

  return err;
}

dberr_t vec_spann_split(dict_table_t *table, THD *thd, uint64_t head_id) {
  Vec_spann_runtime *rt = spann_rt(table);
  if (rt == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  if (!rt->loaded) {
    rw_lock_s_unlock(&rt->latch);
    dberr_t lerr = vec_spann_load(table, thd);
    if (lerr != DB_SUCCESS) {
      return lerr;
    }
    rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  }
  /* A stale job (the generation moved on under it — re-sample, an
  earlier split, a reload) is a clean no-op. */
  if (rt->head_vecs.count(head_id) == 0 || rt->retired.count(head_id) != 0) {
    rw_lock_s_unlock(&rt->latch);
    return DB_SUCCESS;
  }
  const uint32_t dims = rt->dims;
  rw_lock_s_unlock(&rt->latch);

  const uint64_t snap_next = table->vec_next_id.load();

  MDL_ticket *post_mdl = nullptr;
  dict_table_t *postings = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "", thd, &post_mdl);
  if (postings == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }
  MDL_ticket *meta_mdl = nullptr;
  dict_table_t *meta = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "_meta", thd, &meta_mdl);
  MDL_ticket *dead_mdl = nullptr;
  dict_table_t *dead =
      meta == nullptr
          ? nullptr
          : vec_aux_open_for_dml(table, rt->index_id, Vec_index_type::SPANN,
                                 "_dead", thd, &dead_mdl);
  if (meta == nullptr || dead == nullptr) {
    if (meta != nullptr) {
      vec_aux_close_for_dml(meta, thd, &meta_mdl);
    }
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_TABLE_NOT_FOUND;
  }

  /* Scoped dead view (purge is never pinned by the job trx); the job
  transaction itself is viewless. */
  std::unordered_set<uint64_t> dead_set;
  dberr_t err = vec_spann_dead_set_scoped(dead, &dead_set);

  trx_t *trx = trx_allocate_for_background();
  trx_start_internal(trx, UT_LOCATION_HERE);

  /* Snapshot the ONE list — LATEST-read, view only for dead pruning
  (see the resample snapshot comment: a view-based snapshot loses the
  still-uncommitted insert that triggered this very job). Dead labels
  visible to the view are pruned — a split is also a compaction: the
  halves are born clean, the dead copies stay behind in the retired
  list until L2 sweeps it. */
  std::vector<vec_spann_snap_row_t> rows;
  if (err == DB_SUCCESS) {
    err = vec_spann_scan_list(
        trx, postings, head_id, dims, nullptr /* latest */,
        [&](uint64_t label, const float *vec, const byte *ref, ulint ref_len) {
          if (dead_set.count(label) != 0) {
            return;
          }
          vec_spann_snap_row_t r;
          r.label = label;
          r.vec.assign(vec, vec + dims);
          r.ref.assign(reinterpret_cast<const char *>(ref), ref_len);
          rows.push_back(std::move(r));
        });
  }

  hnswlib::L2Space kernel_space(dims);
  const hnswlib::DISTFUNC<float> dist_func = kernel_space.get_dist_func();
  void *dist_param = kernel_space.get_dist_func_param();
  const auto dist = [&](const float *x, const float *y) {
    return dist_func(x, y, dist_param);
  };

  std::vector<float> c1, c2;
  bool splittable = false;
  if (err == DB_SUCCESS && rows.size() >= 2) {
    std::vector<const float *> pts;
    pts.reserve(rows.size());
    for (const auto &r : rows) {
      pts.push_back(r.vec.data());
    }
    splittable = vec_spann_kmeans2(pts, dims, dist, &c1, &c2);
  }

  if (err == DB_SUCCESS && !splittable) {
    /* Under two live points, or all identical: nothing to split. */
    trx_rollback_to_savepoint(trx, nullptr);
    trx_free_for_background(trx);
    vec_aux_close_for_dml(dead, thd, &dead_mdl);
    vec_aux_close_for_dml(meta, thd, &meta_mdl);
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_SUCCESS;
  }

  /* Two centroid heads under fresh counter ids (a centroid is not a
  data point — unlike the sampled builds, split heads are synthetic;
  the shared counter still guarantees identity uniqueness). */
  std::vector<vec_spann_head_t> new_heads;
  if (err == DB_SUCCESS) {
    for (const std::vector<float> &c : {c1, c2}) {
      const uint64_t hid = vec_assign_next_idx_id(table);
      err = vec_spann_meta_insert(trx, meta, VEC_SPANN_META_HEAD, hid,
                                  reinterpret_cast<const byte *>(c.data()),
                                  dims * sizeof(float));
      if (err != DB_SUCCESS) {
        break;
      }
      new_heads.emplace_back(hid, c);
    }
  }

  /* Assign to the nearer half, closure (eps + RNG) between the two. */
  const auto assign_one = [&](uint64_t label, const float *vec, const byte *ref,
                              ulint ref_len) -> dberr_t {
    std::vector<vec_spann_head_cand_t> cands = {
        {dist(vec, new_heads[0].second.data()), new_heads[0].first},
        {dist(vec, new_heads[1].second.data()), new_heads[1].first}};
    if (cands[1].dist < cands[0].dist) {
      std::swap(cands[0], cands[1]);
    }
    std::vector<uint64_t> selected;
    vec_spann_select_heads(
        cands, VEC_SPANN_CLOSURE_EPS, VEC_SPANN_CLOSURE_MAX,
        [&](uint64_t, uint64_t) {
          return dist(new_heads[0].second.data(), new_heads[1].second.data());
        },
        &selected);
    ut_a(!selected.empty());
    for (const uint64_t h : selected) {
      const dberr_t ierr = vec_spann_posting_insert(trx, postings, h, label,
                                                    vec, dims, ref, ref_len);
      if (ierr != DB_SUCCESS) {
        return ierr;
      }
    }
    return DB_SUCCESS;
  };

  if (err == DB_SUCCESS) {
    size_t n_done = 0;
    for (const auto &r : rows) {
      err = assign_one(r.label, r.vec.data(),
                       reinterpret_cast<const byte *>(r.ref.data()),
                       r.ref.size());
      if (err != DB_SUCCESS) {
        break;
      }
      if (++n_done % 256 == 0 && vec_maint_is_canceled(table->id)) {
        err = DB_INTERRUPTED;
        break;
      }
    }
  }

  if (err == DB_SUCCESS && head_id != VEC_SPANN_GENESIS_HEAD_ID) {
    const dberr_t derr =
        vec_spann_meta_delete(trx, meta, VEC_SPANN_META_HEAD, head_id);
    if (derr != DB_SUCCESS && derr != DB_RECORD_NOT_FOUND) {
      err = derr;
    }
  }

  DBUG_EXECUTE_IF("spann_split_pause", {
    const char act[] = "now SIGNAL spann_split_paused WAIT_FOR spann_split_go";
    ut_a(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });

  DBUG_EXECUTE_IF("spann_split_crash_before_commit", DBUG_SUICIDE(););

  if (err == DB_SUCCESS && vec_maint_is_canceled(table->id)) {
    err = DB_INTERRUPTED;
  }

  if (err == DB_SUCCESS) {
    /* Fence, catch-up (this one list's suffix), commit, publish —
    the resample's protocol, one-list-sized. */
    rw_lock_x_lock(&rt->latch, UT_LOCATION_HERE);

    std::vector<vec_spann_snap_row_t> catchup;
    err = vec_spann_scan_list(
        trx, postings, head_id, dims, nullptr /* latest */,
        [&](uint64_t label, const float *vec, const byte *ref, ulint ref_len) {
          vec_spann_snap_row_t r;
          r.label = label;
          r.vec.assign(vec, vec + dims);
          r.ref.assign(reinterpret_cast<const char *>(ref), ref_len);
          catchup.push_back(std::move(r));
        },
        snap_next /* label_gt */);
    for (const auto &r : catchup) {
      if (err != DB_SUCCESS) {
        break;
      }
      err = assign_one(r.label, r.vec.data(),
                       reinterpret_cast<const byte *>(r.ref.data()),
                       r.ref.size());
    }

    if (err == DB_SUCCESS) {
      const trx_id_t retire_id = trx->id;
      trx_commit_for_mysql(trx);
      vec_spann_publish_locked(rt, new_heads, {head_id}, retire_id);
      rw_lock_x_unlock(&rt->latch);
    } else {
      rw_lock_x_unlock(&rt->latch);
      trx_rollback_to_savepoint(trx, nullptr);
    }
  } else {
    trx_rollback_to_savepoint(trx, nullptr);
  }

  trx_free_for_background(trx);
  vec_aux_close_for_dml(dead, thd, &dead_mdl);
  vec_aux_close_for_dml(meta, thd, &meta_mdl);
  vec_aux_close_for_dml(postings, thd, &post_mdl);

  if (err == DB_SUCCESS) {
    MONITOR_INC(MONITOR_VEC_SPANN_SPLITS);
    vec_spann_gc_retired(table);
  }

  return err;
}

dberr_t vec_spann_merge(dict_table_t *table, THD *thd) {
  Vec_spann_runtime *rt = spann_rt(table);
  if (rt == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  if (!rt->loaded) {
    rw_lock_s_unlock(&rt->latch);
    dberr_t lerr = vec_spann_load(table, thd);
    if (lerr != DB_SUCCESS) {
      return lerr;
    }
    rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  }
  std::vector<uint64_t> live;
  for (const auto &hv : rt->head_vecs) {
    if (rt->retired.count(hv.first) == 0) {
      live.push_back(hv.first);
    }
  }
  const uint32_t dims = rt->dims;
  rw_lock_s_unlock(&rt->latch);

  const uint64_t snap_next = table->vec_next_id.load();

  MDL_ticket *post_mdl = nullptr;
  dict_table_t *postings = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "", thd, &post_mdl);
  if (postings == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }
  MDL_ticket *meta_mdl = nullptr;
  dict_table_t *meta = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "_meta", thd, &meta_mdl);
  MDL_ticket *dead_mdl = nullptr;
  dict_table_t *dead =
      meta == nullptr
          ? nullptr
          : vec_aux_open_for_dml(table, rt->index_id, Vec_index_type::SPANN,
                                 "_dead", thd, &dead_mdl);
  if (meta == nullptr || dead == nullptr) {
    if (meta != nullptr) {
      vec_aux_close_for_dml(meta, thd, &meta_mdl);
    }
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_TABLE_NOT_FOUND;
  }

  /* Scoped dead view (purge is never pinned by the job trx); the job
  transaction itself is viewless. */
  std::unordered_set<uint64_t> dead_set;
  dberr_t err = vec_spann_dead_set_scoped(dead, &dead_set);

  trx_t *trx = trx_allocate_for_background();
  trx_start_internal(trx, UT_LOCATION_HERE);

  /* Candidates: LIVE posting count (non-delete-marked minus dead
  labels) below the threshold, two or more of them. */
  uint64_t threshold = VEC_SPANN_MERGE_THRESHOLD;
  DBUG_EXECUTE_IF("spann_merge_threshold_low", threshold = 4;);

  std::vector<uint64_t> smalls;
  if (err == DB_SUCCESS) {
    std::map<uint64_t, uint64_t> live_counts;
    for (const uint64_t h : live) {
      live_counts.emplace(h, 0);
    }
    err = vec_spann_scan_all_postings(
        postings, [&](uint64_t head_id, uint64_t label, const byte *, ulint) {
          auto it = live_counts.find(head_id);
          if (it != live_counts.end() && dead_set.count(label) == 0) {
            ++it->second;
          }
        });
    if (err == DB_SUCCESS) {
      for (const auto &hc : live_counts) {
        if (hc.second < threshold) {
          smalls.push_back(hc.first);
        }
      }
    }
  }

  if (err == DB_SUCCESS && smalls.size() < 2) {
    trx_rollback_to_savepoint(trx, nullptr);
    trx_free_for_background(trx);
    vec_aux_close_for_dml(dead, thd, &dead_mdl);
    vec_aux_close_for_dml(meta, thd, &meta_mdl);
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_SUCCESS;
  }

  const std::unordered_set<uint64_t> small_set(smalls.begin(), smalls.end());

  /* Snapshot the small lists — latest-read, dead pruned, closure
  copies deduped (see the resample snapshot comment for why latest). */
  std::vector<vec_spann_snap_row_t> rows;
  {
    std::unordered_set<uint64_t> seen;
    for (const uint64_t h : smalls) {
      if (err != DB_SUCCESS) {
        break;
      }
      err = vec_spann_scan_list(
          trx, postings, h, dims, nullptr /* latest */,
          [&](uint64_t label, const float *vec, const byte *ref,
              ulint ref_len) {
            if (dead_set.count(label) != 0 || !seen.insert(label).second) {
              return;
            }
            vec_spann_snap_row_t r;
            r.label = label;
            r.vec.assign(vec, vec + dims);
            r.ref.assign(reinterpret_cast<const char *>(ref), ref_len);
            rows.push_back(std::move(r));
          });
    }
  }

  /* The merged head: centroid of the folded heads' vectors — always
  defined, even when every folded list turned out empty (the merged
  list is then just empty, ready for the fence catch-up). Fresh
  counter id as everywhere in phase L. head_vecs reads need no latch
  here: only maintenance jobs mutate it, and jobs are serialized on
  the one maintenance thread — which is us. */
  std::vector<vec_spann_head_t> new_heads;
  if (err == DB_SUCCESS) {
    std::vector<float> centroid(dims, 0.0f);
    for (const uint64_t h : smalls) {
      const std::vector<float> &v = rt->head_vecs.at(h);
      for (uint32_t d = 0; d < dims; ++d) {
        centroid[d] += v[d];
      }
    }
    for (uint32_t d = 0; d < dims; ++d) {
      centroid[d] /= smalls.size();
    }

    const uint64_t hid = vec_assign_next_idx_id(table);
    err = vec_spann_meta_insert(trx, meta, VEC_SPANN_META_HEAD, hid,
                                reinterpret_cast<const byte *>(centroid.data()),
                                dims * sizeof(float));
    if (err == DB_SUCCESS) {
      new_heads.emplace_back(hid, std::move(centroid));
    }
  }

  const auto fold_one = [&](const vec_spann_snap_row_t &r) -> dberr_t {
    return vec_spann_posting_insert(
        trx, postings, new_heads[0].first, r.label, r.vec.data(), dims,
        reinterpret_cast<const byte *>(r.ref.data()), r.ref.size());
  };

  if (err == DB_SUCCESS) {
    size_t n_done = 0;
    for (const auto &r : rows) {
      err = fold_one(r);
      if (err != DB_SUCCESS) {
        break;
      }
      if (++n_done % 256 == 0 && vec_maint_is_canceled(table->id)) {
        err = DB_INTERRUPTED;
        break;
      }
    }
  }

  if (err == DB_SUCCESS) {
    for (const uint64_t h : smalls) {
      if (h == VEC_SPANN_GENESIS_HEAD_ID) {
        continue;
      }
      const dberr_t derr =
          vec_spann_meta_delete(trx, meta, VEC_SPANN_META_HEAD, h);
      if (derr != DB_SUCCESS && derr != DB_RECORD_NOT_FOUND) {
        err = derr;
        break;
      }
    }
  }

  DBUG_EXECUTE_IF("spann_merge_pause", {
    const char act[] = "now SIGNAL spann_merge_paused WAIT_FOR spann_merge_go";
    ut_a(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  });

  if (err == DB_SUCCESS && vec_maint_is_canceled(table->id)) {
    err = DB_INTERRUPTED;
  }

  if (err == DB_SUCCESS) {
    rw_lock_x_lock(&rt->latch, UT_LOCATION_HERE);

    std::vector<vec_spann_snap_row_t> catchup;
    std::unordered_set<uint64_t> caught;
    for (const uint64_t h : smalls) {
      if (err != DB_SUCCESS) {
        break;
      }
      err = vec_spann_scan_list(
          trx, postings, h, dims, nullptr /* latest */,
          [&](uint64_t label, const float *vec, const byte *ref,
              ulint ref_len) {
            if (!caught.insert(label).second) {
              return;
            }
            vec_spann_snap_row_t r;
            r.label = label;
            r.vec.assign(vec, vec + dims);
            r.ref.assign(reinterpret_cast<const char *>(ref), ref_len);
            catchup.push_back(std::move(r));
          },
          snap_next /* label_gt */);
    }
    for (const auto &r : catchup) {
      if (err != DB_SUCCESS) {
        break;
      }
      err = fold_one(r);
    }

    if (err == DB_SUCCESS) {
      const trx_id_t retire_id = trx->id;
      trx_commit_for_mysql(trx);
      vec_spann_publish_locked(rt, new_heads, smalls, retire_id);
      rw_lock_x_unlock(&rt->latch);
    } else {
      rw_lock_x_unlock(&rt->latch);
      trx_rollback_to_savepoint(trx, nullptr);
    }
  } else {
    trx_rollback_to_savepoint(trx, nullptr);
  }

  trx_free_for_background(trx);
  vec_aux_close_for_dml(dead, thd, &dead_mdl);
  vec_aux_close_for_dml(meta, thd, &meta_mdl);
  vec_aux_close_for_dml(postings, thd, &post_mdl);

  if (err == DB_SUCCESS) {
    MONITOR_INC(MONITOR_VEC_SPANN_MERGES);
    vec_spann_gc_retired(table);
  }

  return err;
}

dberr_t vec_spann_gc(dict_table_t *table, THD *thd) {
  Vec_spann_runtime *rt = spann_rt(table);
  if (rt == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  if (!rt->loaded) {
    rw_lock_s_unlock(&rt->latch);
    dberr_t lerr = vec_spann_load(table, thd);
    if (lerr != DB_SUCCESS) {
      return lerr;
    }
    rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  }
  rw_lock_s_unlock(&rt->latch);

  /* Drain retirements first: whatever clears here becomes sweepable
  garbage below. */
  vec_spann_gc_retired(table);

  /* Heads still reachable through the RAM graph (live AND still-
  retired) — their lists are NOT garbage. */
  std::unordered_set<uint64_t> ram_heads;
  rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  for (const auto &hv : rt->head_vecs) {
    ram_heads.insert(hv.first);
  }
  rw_lock_s_unlock(&rt->latch);

  MDL_ticket *post_mdl = nullptr;
  dict_table_t *postings = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "", thd, &post_mdl);
  if (postings == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }
  MDL_ticket *dead_mdl = nullptr;
  dict_table_t *dead = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "_dead", thd, &dead_mdl);
  if (dead == nullptr) {
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_TABLE_NOT_FOUND;
  }

  /* GC-able dead labels: the deleter is visible to the OLDEST active
  view, i.e. to everyone — sweeping such a label changes no reader's
  answer (old views keep the delete-marked versions via MVCC anyway;
  this gate exists so an uncommitted or freshly-committed DELETE is
  never the basis for physical removal). The same all-seeing view
  drives the orphan probes below. */
  dberr_t err;
  ReadView oldest;
  std::unordered_set<uint64_t> gc_dead;
  {
    trx_sys->mvcc->clone_oldest_view(&oldest);
    err = vec_spann_load_dead_set(dead, &oldest, &gc_dead);
  }

  /* ONE discovery scan: garbage-list rows (head unreachable from the
  RAM graph — no reader can probe them) and swept-dead rows go
  straight to the victim list; everything else is a survivor whose
  copy locations and row_ref we keep for the orphan probe. */
  struct gc_survivor_t {
    std::string ref;
    std::vector<uint64_t> heads;
  };
  std::vector<std::pair<uint64_t, uint64_t>> victims;
  std::unordered_map<uint64_t, gc_survivor_t> survivors;
  if (err == DB_SUCCESS) {
    err = vec_spann_scan_all_postings(
        postings,
        [&](uint64_t head_id, uint64_t label, const byte *ref, ulint ref_len) {
          if (ram_heads.count(head_id) == 0 || gc_dead.count(label) != 0) {
            victims.emplace_back(head_id, label);
            return;
          }
          gc_survivor_t &sv = survivors[label];
          if (sv.heads.empty()) {
            sv.ref.assign(reinterpret_cast<const char *>(ref), ref_len);
          }
          sv.heads.push_back(head_id);
        });
  }

  /* The ORPHAN rule: a surviving not-dead label whose base row is
  physically gone (its insert rolled back after a maintenance job
  copied the posting) or whose base PK now belongs to a different row
  identity (PK reuse after such a rollback — the visible version's
  vec_idx_id differs). Without this rule such copies would survive and
  propagate through every future snapshot. One base-PK point probe per
  distinct label, same cost class as the discovery scan. */
  if (err == DB_SUCCESS) {
    for (const auto &sv : survivors) {
      bool orphan = false;
      err = vec_spann_base_probe(
          table, reinterpret_cast<const byte *>(sv.second.ref.data()),
          sv.second.ref.size(), &oldest, sv.first, &orphan);
      if (err != DB_SUCCESS) {
        break;
      }
      if (orphan) {
        for (const uint64_t h : sv.second.heads) {
          victims.emplace_back(h, sv.first);
        }
      }
    }
  }

  if (err == DB_SUCCESS && victims.empty() && gc_dead.empty()) {
    vec_aux_close_for_dml(dead, thd, &dead_mdl);
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_SUCCESS;
  }

  trx_t *trx = nullptr;
  if (err == DB_SUCCESS) {
    trx = trx_allocate_for_background();
    trx_start_internal(trx, UT_LOCATION_HERE);

    size_t n_done = 0;
    for (const auto &v : victims) {
      const dberr_t derr =
          vec_spann_posting_delete(trx, postings, v.first, v.second);
      if (derr != DB_SUCCESS && derr != DB_RECORD_NOT_FOUND) {
        err = derr;
        break;
      }
      if (++n_done % 256 == 0 && vec_maint_is_canceled(table->id)) {
        err = DB_INTERRUPTED;
        break;
      }
    }

    /* Retire the _dead rows themselves: every copy of these labels is
    now delete-marked (live lists, retired lists and garbage lists all
    passed through the scan above). */
    if (err == DB_SUCCESS) {
      for (const uint64_t label : gc_dead) {
        const dberr_t derr = vec_spann_dead_delete(trx, dead, label);
        if (derr != DB_SUCCESS && derr != DB_RECORD_NOT_FOUND) {
          err = derr;
          break;
        }
      }
    }

    if (err == DB_SUCCESS) {
      trx_commit_for_mysql(trx);
    } else {
      trx_rollback_to_savepoint(trx, nullptr);
    }
    trx_free_for_background(trx);
  }

  vec_aux_close_for_dml(dead, thd, &dead_mdl);
  vec_aux_close_for_dml(postings, thd, &post_mdl);

  if (err == DB_SUCCESS) {
    MONITOR_INC(MONITOR_VEC_SPANN_GCS);
  }

  return err;
}

/** One NPA violation: a live label with no posting in its nearest
live head's list. */
struct vec_spann_npa_violation_t {
  uint64_t label;
  uint64_t nearest_head;
  std::vector<float> vec;
  std::string ref;
};

/** Collect the NPA (nearest-posting-assignment) violations: for every
live label across the LIVE lists, its nearest live head must hold a
copy. The write path routes every insert to its nearest live head and
splits re-assign their list optimally, so violations come from MERGES
— a centroid between two distant folded lists can leave their rows
nearer to some third head — and that is why the invariant needs a
repair job at all. Latest-read like every maintenance scan; dead
labels (visible to a fresh view) excluded. */
static dberr_t vec_spann_collect_npa(
    dict_table_t *table, THD *thd,
    std::vector<vec_spann_npa_violation_t> *out) {
  out->clear();

  Vec_spann_runtime *rt = spann_rt(table);
  if (rt == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  if (!rt->loaded) {
    rw_lock_s_unlock(&rt->latch);
    dberr_t lerr = vec_spann_load(table, thd);
    if (lerr != DB_SUCCESS) {
      return lerr;
    }
    rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  }
  std::vector<uint64_t> live;
  for (const auto &hv : rt->head_vecs) {
    if (rt->retired.count(hv.first) == 0) {
      live.push_back(hv.first);
    }
  }
  const uint32_t dims = rt->dims;
  rw_lock_s_unlock(&rt->latch);

  MDL_ticket *post_mdl = nullptr;
  dict_table_t *postings = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "", thd, &post_mdl);
  if (postings == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }
  MDL_ticket *dead_mdl = nullptr;
  dict_table_t *dead = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "_dead", thd, &dead_mdl);
  if (dead == nullptr) {
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_TABLE_NOT_FOUND;
  }

  /* Scoped dead view; the scan companion trx below is viewless (it
  exists only as LOB-read context), so an NPA pass over a large index
  never pins purge. */
  std::unordered_set<uint64_t> dead_set;
  dberr_t err = vec_spann_dead_set_scoped(dead, &dead_set);

  trx_t *dtrx = trx_allocate_for_background();
  trx_start_internal_read_only(dtrx, UT_LOCATION_HERE);

  /* label -> (vec, ref, lists that hold a copy), live lists only. */
  struct npa_row_t {
    std::vector<float> vec;
    std::string ref;
    std::unordered_set<uint64_t> lists;
  };
  std::unordered_map<uint64_t, npa_row_t> labels;

  for (const uint64_t h : live) {
    if (err != DB_SUCCESS) {
      break;
    }
    err = vec_spann_scan_list(
        dtrx, postings, h, dims, nullptr /* latest */,
        [&](uint64_t label, const float *vec, const byte *ref, ulint ref_len) {
          if (dead_set.count(label) != 0) {
            return;
          }
          npa_row_t &r = labels[label];
          if (r.lists.empty()) {
            r.vec.assign(vec, vec + dims);
            r.ref.assign(reinterpret_cast<const char *>(ref), ref_len);
          }
          r.lists.insert(h);
        });
  }

  trx_commit_for_mysql(dtrx);
  trx_free_for_background(dtrx);

  if (err == DB_SUCCESS) {
    rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
    Vec_live_head_filter filter(rt->retired);
    for (auto &lr : labels) {
      try {
        const auto found = rt->heads->searchKnnCloserFirst(
            lr.second.vec.data(), 1, rt->retired.empty() ? nullptr : &filter);
        if (!found.empty() && lr.second.lists.count(found[0].second) == 0) {
          vec_spann_npa_violation_t v;
          v.label = lr.first;
          v.nearest_head = found[0].second;
          v.vec = std::move(lr.second.vec);
          v.ref = std::move(lr.second.ref);
          out->push_back(std::move(v));
        }
      } catch (...) {
        err = DB_OUT_OF_MEMORY;
        break;
      }
    }
    rw_lock_s_unlock(&rt->latch);
  }

  vec_aux_close_for_dml(dead, thd, &dead_mdl);
  vec_aux_close_for_dml(postings, thd, &post_mdl);
  return err;
}

dberr_t vec_spann_npa_violations(dict_table_t *table, THD *thd,
                                 uint64_t *count) {
  std::vector<vec_spann_npa_violation_t> v;
  const dberr_t err = vec_spann_collect_npa(table, thd, &v);
  *count = v.size();
  return err;
}

dberr_t vec_spann_reassign(dict_table_t *table, THD *thd) {
  Vec_spann_runtime *rt = spann_rt(table);
  if (rt == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  std::vector<vec_spann_npa_violation_t> violations;
  dberr_t err = vec_spann_collect_npa(table, thd, &violations);
  if (err != DB_SUCCESS || violations.empty()) {
    return err;
  }

  MDL_ticket *post_mdl = nullptr;
  dict_table_t *postings = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "", thd, &post_mdl);
  if (postings == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  /* Appends only, no fence, no publish: the head set does not change,
  so readers need no transition — each copy becomes visible to new
  views at commit like any row. Nothing here can create a NEW
  violation (concurrent inserts route to their nearest live head by
  construction), so one pass converges. */
  trx_t *trx = trx_allocate_for_background();
  trx_start_internal(trx, UT_LOCATION_HERE);

  size_t n_done = 0;
  for (const auto &v : violations) {
    const dberr_t ierr = vec_spann_posting_insert(
        trx, postings, v.nearest_head, v.label, v.vec.data(), rt->dims,
        reinterpret_cast<const byte *>(v.ref.data()), v.ref.size());
    /* A racing copy (an UPDATE re-append, say) is fine — the goal is
    presence, not authorship. */
    if (ierr != DB_SUCCESS && ierr != DB_DUPLICATE_KEY) {
      err = ierr;
      break;
    }
    if (++n_done % 256 == 0 && vec_maint_is_canceled(table->id)) {
      err = DB_INTERRUPTED;
      break;
    }
  }

  if (err == DB_SUCCESS) {
    trx_commit_for_mysql(trx);
  } else {
    trx_rollback_to_savepoint(trx, nullptr);
  }
  trx_free_for_background(trx);
  vec_aux_close_for_dml(postings, thd, &post_mdl);

  if (err == DB_SUCCESS) {
    MONITOR_INC(MONITOR_VEC_SPANN_REASSIGNS);

#ifdef UNIV_DEBUG
    /* THE NPA INVARIANT, asserted: after a repair pass no live label
    may lack a copy in its nearest live head's list. Safe to recompute
    live — concurrent user inserts are NPA-correct at birth and only
    this (single) maintenance thread reshapes the head set. */
    std::vector<vec_spann_npa_violation_t> check;
    ut_ad(vec_spann_collect_npa(table, thd, &check) != DB_SUCCESS ||
          check.empty());
#endif /* UNIV_DEBUG */
  }

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

dberr_t vec_spann_knn(dict_table_t *table, THD *thd, const float *query,
                      uint32_t dims, size_t k, size_t ef [[maybe_unused]],
                      std::vector<vec_knn_hit_t> *hits,
                      const std::unordered_set<uint64_t> *exclude) {
  ut_a(hits != nullptr);
  hits->clear();

  Vec_spann_runtime *rt = spann_rt(table);
  if (rt == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }
  if (dims != 0 && dims != rt->dims) {
    return DB_CORRUPTION;
  }
  if (k == 0) {
    return DB_SUCCESS;
  }

  /* The CALLER's transaction and read view: the posting scan must sit
  at the reader's MVCC position — that is the whole MVCC story of this
  index (design §5). The view is opened here if the statement has not
  read anything yet (exactly what row_search_mvcc would do first);
  under READ UNCOMMITTED / SERIALIZABLE this gives candidate discovery
  snapshot semantics, while the per-hit base fetch above us still
  applies the session's real isolation. */
  trx_t *trx = thd_to_trx(thd);
  ut_a(trx != nullptr);
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  ReadView *view = trx_assign_read_view(trx);

  MDL_ticket *post_mdl = nullptr;
  dict_table_t *postings = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "", thd, &post_mdl);
  if (postings == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }
  MDL_ticket *dead_mdl = nullptr;
  dict_table_t *dead = vec_aux_open_for_dml(
      table, rt->index_id, Vec_index_type::SPANN, "_dead", thd, &dead_mdl);
  if (dead == nullptr) {
    vec_aux_close_for_dml(postings, thd, &post_mdl);
    return DB_TABLE_NOT_FOUND;
  }

  MONITOR_INC(MONITOR_VEC_SPANN_SEARCHES);

  std::unordered_set<uint64_t> dead_set;
  dberr_t err = vec_spann_load_dead_set(dead, view, &dead_set);

  rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  if (err == DB_SUCCESS && !rt->loaded) {
    rw_lock_s_unlock(&rt->latch);
    err = vec_spann_load(table, thd);
    rw_lock_s_lock(&rt->latch, UT_LOCATION_HERE);
  }

  if (err == DB_SUCCESS) {
    const hnswlib::DISTFUNC<float> dist_func = rt->space->get_dist_func();
    void *dist_param = rt->space->get_dist_func_param();

    /* All heads, closer-first: the probe ORDER. How FAR down the list
    we go is decided per query below. */
    std::vector<std::pair<float, uint64_t>> head_order;
    try {
      const auto found =
          rt->heads->searchKnnCloserFirst(query, rt->head_vecs.size());
      head_order.reserve(found.size());
      for (const auto &h : found) {
        head_order.emplace_back(h.first, static_cast<uint64_t>(h.second));
      }
    } catch (...) {
      err = DB_OUT_OF_MEMORY;
    }

    /* Candidates: label -> (exact distance, row_ref). Closure copies
    dedup here — copies carry identical payloads, first hit wins. */
    std::unordered_map<uint64_t, std::pair<float, std::string>> cands;

    for (size_t i = 0; err == DB_SUCCESS && i < head_order.size(); ++i) {
      /* Stop when the probe floor is met, k candidates exist AND the
      next list's head is beyond the probe tolerance — nearer lists
      were all scanned. */
      if (i >= VEC_SPANN_PROBE_MIN && cands.size() >= k &&
          head_order[i].first >
              (1.0f + VEC_SPANN_PROBE_EPS) * head_order[0].first) {
        break;
      }

      MONITOR_INC(MONITOR_VEC_SPANN_PROBES);
      err = vec_spann_scan_list(
          trx, postings, head_order[i].second, rt->dims, view,
          [&](uint64_t label, const float *vec, const byte *ref,
              ulint ref_len) {
            if (dead_set.count(label) != 0 ||
                (exclude != nullptr && exclude->count(label) != 0) ||
                cands.count(label) != 0) {
              return;
            }
            const float d = dist_func(query, vec, dist_param);
            cands.emplace(
                label, std::make_pair(
                           d, std::string(reinterpret_cast<const char *>(ref),
                                          ref_len)));
          });
    }

    if (err == DB_SUCCESS) {
      /* Exact-distance top-k, closer-first. */
      std::vector<std::pair<float, uint64_t>> order;
      order.reserve(cands.size());
      for (const auto &c : cands) {
        order.emplace_back(c.second.first, c.first);
      }
      std::sort(order.begin(), order.end());
      if (order.size() > k) {
        order.resize(k);
      }
      hits->reserve(order.size());
      for (const auto &o : order) {
        vec_knn_hit_t hit;
        hit.label = o.second;
        hit.dist = o.first;
        hit.row_ref = std::move(cands.at(o.second).second);
        hits->push_back(std::move(hit));
      }
    }
  }

  rw_lock_s_unlock(&rt->latch);
  vec_aux_close_for_dml(dead, thd, &dead_mdl);
  vec_aux_close_for_dml(postings, thd, &post_mdl);

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
  stats->n_retired = rt->retired.size();
  stats->dead_appends = rt->dead_appends.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> g(rt->counter_mutex);
    stats->list_appends.assign(rt->list_appends.begin(),
                               rt->list_appends.end());
  }
  rw_lock_s_unlock(&rt->latch);
  return true;
}
