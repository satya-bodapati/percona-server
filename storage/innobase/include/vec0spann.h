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

/** @file include/vec0spann.h
TYPE spann (SPANN plan, S2+): the build pass and the pure
closure-assignment algorithm.

The top half of this header is deliberately engine-free (only <cstdint>
and <vector>) so the closure/RNG selection — the correctness-critical
piece — is unit-testable from gunit without a server
(unittest/gunit/innodb/vec0spann-t.cc). The engine-facing build entry
point below it uses forward declarations only. */

#ifndef vec0spann_h
#define vec0spann_h

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db0err.h"
#include "univ.i"

struct vec_knn_hit_t; /* vec0aux.h */

/* --------------------------------------------------------------------
Meta-table row types: the `mtype` half of the _meta PK (mtype, id).
Values are the on-disk encoding (design doc §2) — never renumber. */

/** A head: id = the head's label, mval = its vector bytes. The HEAD
range is contiguous under the (mtype, id) PK — one range scan loads
every head at graph-rebuild time (S6). */
constexpr uint8_t VEC_SPANN_META_HEAD = 0;
/** Index-level config rows (unused until LIRE). */
constexpr uint8_t VEC_SPANN_META_CONFIG = 1;
/** Snapshot warm-up chunks (deferred by explicit decision). */
constexpr uint8_t VEC_SPANN_META_SNAPSHOT = 2;

/* --------------------------------------------------------------------
Build-pass constants. INTERNAL in S2 — user knobs (a WITH() surface or
sysvars) arrive only after benchmarks say which of these matter. */

/** Heads as a percentage of indexed rows (design §2: 10-20% of N). */
constexpr size_t VEC_SPANN_HEADS_PCT = 15;

/** Maximum closure copies per vector (design §3: ~8, RNG-pruned). Also
the head-kNN width of the assignment query. */
constexpr size_t VEC_SPANN_CLOSURE_MAX = 8;

/** Closure tolerance: a candidate head qualifies while its distance is
within (1 + eps) of the nearest head's. Applied to the space's RAW
distance (squared L2 for L2Space), so as a true-distance ratio this is
sqrt(1 + eps) — the constant is tuned in that knowledge. */
constexpr float VEC_SPANN_CLOSURE_EPS = 0.25f;

/** Query-time probe tolerance (S5): after enough candidates are
collected, keep probing lists whose head distance is within
(1 + eps) of the nearest head's — deliberately wider than the closure
eps (a query near a list boundary needs neighbors the closure did not
replicate). Same raw-distance semantics as VEC_SPANN_CLOSURE_EPS.
INTERNAL until benchmarks; the natural user knob for recall/latency
(S7). */
constexpr float VEC_SPANN_PROBE_EPS = 1.0f;

/** Minimum lists probed per query (capped by the head count). The
multiplicative eps above collapses when the query lands almost ON a
head (d1 -> 0 shrinks the tolerance to nothing, yet the true neighbors
straddle the adjacent lists), so a floor of nearest lists is always
scanned. Like every probe constant this trades latency for recall —
the search is approximate by construction. */
constexpr size_t VEC_SPANN_PROBE_MIN = 2;

/** L1 split trigger: a list whose since-load append counter crosses a
multiple of this enqueues a SPLIT for itself (multiples, not a single
edge, so an aborted split re-arms). INTERNAL; the
"spann_split_threshold_low" DBUG knob drops it to 8 for tests. */
constexpr uint64_t VEC_SPANN_SPLIT_THRESHOLD = 4096;

/** Deterministic 2-means for one posting list (L1 split): farthest-
point initialization (no RNG — the point farthest from the mean, then
the point farthest from that), then at most `VEC_SPANN_KMEANS_ROUNDS`
Lloyd's rounds of assign-to-nearer / recompute-mean. Pure and
gunit-testable like vec_spann_select_heads.

@param[in]  pts   the list's vectors (borrowed pointers)
@param[in]  dims  vector dimensions
@param[in]  dist  float(const float*, const float*): the space's raw
                  distance kernel
@param[out] c1    first centroid (replaced)
@param[out] c2    second centroid (replaced)
@return false when the list cannot split (fewer than two points, or
all points identical — the centroids would coincide and every row
would replicate into both halves, splitting nothing) */
constexpr unsigned VEC_SPANN_KMEANS_ROUNDS = 8;

template <typename Dist_fn>
inline bool vec_spann_kmeans2(const std::vector<const float *> &pts,
                              uint32_t dims, Dist_fn dist,
                              std::vector<float> *c1, std::vector<float> *c2) {
  if (pts.size() < 2) {
    return false;
  }

  std::vector<float> mean(dims, 0.0f);
  for (const float *p : pts) {
    for (uint32_t d = 0; d < dims; ++d) {
      mean[d] += p[d];
    }
  }
  for (uint32_t d = 0; d < dims; ++d) {
    mean[d] /= pts.size();
  }

  size_t a = 0;
  float best = -1.0f;
  for (size_t i = 0; i < pts.size(); ++i) {
    const float dm = dist(pts[i], mean.data());
    if (dm > best) {
      best = dm;
      a = i;
    }
  }
  size_t b = 0;
  best = -1.0f;
  for (size_t i = 0; i < pts.size(); ++i) {
    const float da = dist(pts[i], pts[a]);
    if (da > best) {
      best = da;
      b = i;
    }
  }
  if (dist(pts[a], pts[b]) == 0.0f) {
    return false; /* all points identical */
  }

  c1->assign(pts[a], pts[a] + dims);
  c2->assign(pts[b], pts[b] + dims);

  std::vector<uint8_t> side(pts.size(), 0);
  for (unsigned round = 0; round < VEC_SPANN_KMEANS_ROUNDS; ++round) {
    bool changed = false;
    for (size_t i = 0; i < pts.size(); ++i) {
      const uint8_t s =
          dist(pts[i], c2->data()) < dist(pts[i], c1->data()) ? 1 : 0;
      if (s != side[i]) {
        side[i] = s;
        changed = true;
      }
    }
    if (!changed && round != 0) {
      break;
    }
    std::vector<float> m1(dims, 0.0f), m2(dims, 0.0f);
    size_t n1 = 0, n2 = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
      std::vector<float> &m = side[i] == 0 ? m1 : m2;
      (side[i] == 0 ? n1 : n2) += 1;
      for (uint32_t d = 0; d < dims; ++d) {
        m[d] += pts[i][d];
      }
    }
    /* An emptied side keeps its previous centroid. */
    if (n1 != 0) {
      for (uint32_t d = 0; d < dims; ++d) {
        (*c1)[d] = m1[d] / n1;
      }
    }
    if (n2 != 0) {
      for (uint32_t d = 0; d < dims; ++d) {
        (*c2)[d] = m2[d] / n2;
      }
    }
  }

  return true;
}

/** One head candidate for closure selection: the space's raw distance
from the vector being assigned to this head. */
struct vec_spann_head_cand_t {
  float dist;
  uint64_t head_id;
};

/** Select the heads a vector is replicated to (SPANN closure with the
RNG pruning rule).

`cands` must be sorted closer-first (searchKnnCloserFirst order). The
nearest head is always selected. A farther candidate h is selected iff
 - dist(x, h) <= (1 + eps) * dist(x, nearest)   [closure tolerance], and
 - no already-selected head s has head_dist(s, h) < dist(x, h)
   [RNG rule: h sits behind s as seen from x — a copy there is
   redundant, s's list already covers this boundary].
At most `max_copies` heads are selected.

@param[in]  cands       candidate heads, closer-first
@param[in]  eps         closure tolerance on the space's raw distance
@param[in]  max_copies  replication cap (>= 1)
@param[in]  head_dist   float(uint64_t a, uint64_t b): raw distance
                        between two heads' vectors, same metric as
                        cands[].dist
@param[out] out         selected head ids, nearest first (replaced) */
template <typename Head_dist_fn>
inline void vec_spann_select_heads(
    const std::vector<vec_spann_head_cand_t> &cands, float eps,
    size_t max_copies, Head_dist_fn head_dist, std::vector<uint64_t> *out) {
  out->clear();
  if (cands.empty() || max_copies == 0) {
    return;
  }

  out->push_back(cands[0].head_id);
  const float limit = (1.0f + eps) * cands[0].dist;

  for (size_t i = 1; i < cands.size() && out->size() < max_copies; ++i) {
    if (cands[i].dist > limit) {
      break; /* sorted: everything after is farther still */
    }
    bool occluded = false;
    for (const uint64_t sel : *out) {
      if (head_dist(sel, cands[i].head_id) < cands[i].dist) {
        occluded = true;
        break;
      }
    }
    if (!occluded) {
      out->push_back(cands[i].head_id);
    }
  }
}

/* --------------------------------------------------------------------
Engine-facing entry points (implemented in vec/vec0spann.cc). */

struct dict_table_t;
struct dict_index_t;
struct trx_t;
class THD;
class Vector_index;

/** head_id 0 is reserved — labels start at 1 (vec_assign_next_idx_id),
so no head that is a sampled/promoted data point can be 0. It denotes
the IMPLICIT bootstrap head: a zero-vector head that exists exactly
when _meta has no HEAD rows (a fresh CREATE-then-INSERT table before
any build). RAM-only — no _meta row, so nothing to keep undo-consistent
with user transactions — and deterministic across restarts because the
rule is a pure function of the (empty) HEAD range. Every insert routes
to it while it is the only head; the first build pass replaces the aux
set wholesale, and LIRE later splits skewed lists, so it retires
naturally. */
constexpr uint64_t VEC_SPANN_GENESIS_HEAD_ID = 0;

/** Runtime stats for observability (spann_dump). */
struct vec_spann_stats_t {
  /** heads in the RAM graph (includes the implicit genesis head and
  not-yet-GC'd retired heads) */
  size_t n_heads;
  /** retired heads still probed for old readers (phase L) */
  size_t n_retired;
  /** dead-label inserts since this runtime loaded (LIRE feed) */
  uint64_t dead_appends;
  /** per-list posting appends since this runtime loaded (LIRE feed) */
  std::vector<std::pair<uint64_t, uint64_t>> list_appends;
};

/** Build a TYPE spann index from a clustered scan of the base table —
the INPLACE re-ADD build pass (Vector_index::build for spann, S2).

Single scan (vec_base_collect_rows), random head sample
(VEC_SPANN_HEADS_PCT; fixed seed under the "spann_build_seed_42" DBUG
knob for deterministic tests), head vectors written as _meta (HEAD, id)
rows, an in-RAM hnswlib graph over the heads (private to the build, no
persistence callbacks), then every row is assigned to its nearest head
plus closure copies (vec_spann_select_heads) as posting rows.

All aux rows ride `trx` (the ALTER's transaction): a later ALTER
failure rolls them back with it, and the aux set itself is dropped by
the existing error paths — exactly the hnsw vec_build_index contract.

@param[in,out] trx       the ALTER's transaction
@param[in]     table     base table (quiesced: SHARED lock or better)
@param[in]     vec_index the spann index being added
@param[in]     dims      vector dimensions (from the SQL layer)
@param[in]     M         head-graph HNSW M (spann has no WITH() surface
                         in S2, so this is the parser default)
@param[in]     ef_construction  head-graph build width (ditto)
@param[in]     thd       session (aux open fallback)
@return DB_SUCCESS or error */
dberr_t vec_spann_build_index(trx_t *trx, dict_table_t *table,
                              const dict_index_t *vec_index, uint32_t dims,
                              int M, int ef_construction, THD *thd);

/** Get-or-create the spann runtime companion on table->vec (lazy;
thread-safe via dict_sys mutex — the vec_open analog). Parameters are
only applied on creation; the head graph is loaded separately
(vec_spann_load). `impl` is stored in Vec_runtime::impl before
publication (SPANN R2). */
void vec_spann_open(dict_table_t *table, const Vector_index *impl,
                    uint16_t field_no, uint32_t dims, int M,
                    int ef_construction);

/** Build (or rebuild) the RAM head graph from the _meta HEAD range
under the runtime's X latch; no-op when already loaded. Zero HEAD rows
=> the implicit genesis head (see VEC_SPANN_GENESIS_HEAD_ID). Called
at table open and lazily from the insert path.
@return DB_SUCCESS or error */
dberr_t vec_spann_load(dict_table_t *table, THD *thd);

/** Index one new point (SPANN S3, the write_row hook): head kNN on
the RAM graph, closure selection (vec_spann_select_heads), then one
posting-row INSERT per selected head on the USER transaction. Inserts
only — no aux updates, no graph mutation, no rollback tracking: a
rollback is pure undo (the all-DML-are-inserts posture; contrast
vec_insert_point's vec_trx_record machinery).
@return DB_SUCCESS or error */
dberr_t vec_spann_insert_point(trx_t *trx, dict_table_t *table, THD *thd,
                               uint64_t label, const float *vec_data,
                               const byte *row_ref, ulint row_ref_len);

/** Retire one label (SPANN S4, the DELETE hook and the delete half of
an UPDATE): one INSERT into _dead on the USER transaction — see
vec_spann_dead_insert for the visibility/rollback contract. Touches
neither the head graph nor the postings; needs no load. */
dberr_t vec_spann_remove_point(trx_t *trx, dict_table_t *table, THD *thd,
                               uint64_t label);

/** Approximate kNN (SPANN S5, the read path): rank heads on the RAM
graph, scan the nearest posting lists UNDER THE CALLER'S READ VIEW
(widening per VEC_SPANN_PROBE_EPS until k candidates exist), drop
labels dead under the same view, dedup closure copies by label, and
return the exact-distance top-k closer-first. `hits` carry the
posting's row_ref; the caller fetches base rows through its own
read machinery, so a hit it cannot see resolves to a skip there.
`exclude` is the resume hook (labels already returned). `ef` is
accepted for seam compatibility and unused — probe width is the
spann recall knob, not ef.
@return DB_SUCCESS or error */
dberr_t vec_spann_knn(dict_table_t *table, THD *thd, const float *query,
                      uint32_t dims, size_t k, size_t ef,
                      std::vector<vec_knn_hit_t> *hits,
                      const std::unordered_set<uint64_t> *exclude);

/** L0: background global re-sample, the maintenance-thread job that
rebuilds the head set from the CURRENT postings (never the base
table): snapshot every live label under the job trx's read view,
sample a new head set (fresh counter ids — head identity and label
identity share the crash-safe counter), write the new generation's
_meta rows and postings plus the old generation's _meta deletes all on
that ONE transaction, catch the concurrent-insert suffix
(label > snapshot boundary) under a brief X fence on the runtime
latch, commit, and publish: new heads live, old heads retired-in-RAM
for old readers until GC. Cancellation (vec_maint_cancel_and_wait) and
crash are plain rollbacks — the old world stands.
@return DB_SUCCESS, DB_INTERRUPTED on cancellation, or error */
dberr_t vec_spann_resample(dict_table_t *table, THD *thd);

/** L1: split ONE oversized list, the maintenance-thread job the
insert path enqueues when a list's append counter crosses
VEC_SPANN_SPLIT_THRESHOLD. Same one-transaction / fence / publish
protocol as the re-sample, scoped to a single list: snapshot the list
under the job trx's view (dead labels pruned — a split is also a
compaction), k-means(2) it, two fresh-counter-id centroid heads, the
old head's _meta row deleted, suffix catch-up under the fence, retire.
A list that cannot split (all points identical) is a clean no-op.
@return DB_SUCCESS, DB_INTERRUPTED on cancellation, or error */
dberr_t vec_spann_split(dict_table_t *table, THD *thd, uint64_t head_id);

/** Free the runtime (head graph, latch, counters). Safe on tables
that never opened one — the vec_close analog. */
void vec_spann_close(dict_table_t *table);

/** Snapshot the runtime's observability stats into `stats`.
@return false if `table` has no loaded spann runtime */
bool vec_spann_runtime_stats(dict_table_t *table, vec_spann_stats_t *stats);

#endif /* vec0spann_h */
