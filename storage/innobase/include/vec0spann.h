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
#include <vector>

#include "db0err.h"

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

#endif /* vec0spann_h */
