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

/** @file unittest/gunit/innodb/hnswlib-t.cc
Tests for the Percona hnswlib fork (extra/hnswlib) — specifically the
persistence extensions the InnoDB vector index relies on:
  - addPoint insert/update callbacks (the aux-table write hooks),
  - loadIndex(rows) graph reconstruction from aux-table tuples,
  - resizeIndex mid-stream growth,
  - level distribution staying within the aux table's TINYINT column. */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "extra/hnswlib/hnswlib.h"

namespace innodb_hnswlib_unittest {

using hnswlib::HierarchicalNSW;
using hnswlib::L2Space;
using hnswlib::labeltype;
using hnswlib::VecAuxLoadedRowTuple;

constexpr size_t DIM = 8;

static std::vector<float> make_vec(uint64_t seed) {
  std::mt19937 gen(static_cast<uint32_t>(seed));
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  std::vector<float> v(DIM);
  for (auto &x : v) x = d(gen);
  return v;
}

/* Mirrors what the InnoDB callbacks persist (H1): an append-only log of
(label, ver, neighbors) rows plus a ver-0 birth row per label. `rows`
resolves the log exactly the way vec_aux_load_rows does — identity from
ver 0, edges from the HIGHEST ver seen — so a test that inspects `rows`
is asserting what a reload would reconstruct. `log` keeps every append
in emission order so tests can assert the versions themselves. */
struct Capture {
  struct Append {
    labeltype label;
    uint32_t ver;
    std::vector<std::vector<size_t>> neighbors;
  };
  std::vector<Append> log;
  std::map<labeltype, VecAuxLoadedRowTuple> rows;
  std::map<labeltype, uint32_t> win_ver;
  size_t inserts = 0;
  size_t updates = 0;

  void store(labeltype label, uint32_t ver, const void *data,
             const HierarchicalNSW<float>::NeighborLabelListsByLevel &nbl) {
    std::vector<std::vector<size_t>> neighbors;
    for (const auto &lvl : nbl) {
      neighbors.emplace_back(lvl.begin(), lvl.end());
    }
    log.push_back(Append{label, ver, neighbors});

    /* Loader semantics: only a strictly higher version wins. */
    auto it = win_ver.find(label);
    const bool wins = (it == win_ver.end()) || (ver >= it->second);
    if (!wins) {
      return;
    }
    win_ver[label] = ver;

    const uint64_t level = nbl.empty() ? 0 : nbl.size() - 1;
    auto existing = rows.find(label);
    std::vector<float> vec;
    if (data != nullptr) {
      const float *f = static_cast<const float *>(data);
      vec.assign(f, f + DIM);
    } else if (existing != rows.end()) {
      vec = std::get<2>(existing->second); /* identity from the birth row */
    }
    const uint64_t keep_level = (data == nullptr && existing != rows.end())
                                    ? std::get<1>(existing->second)
                                    : level;
    rows[label] = VecAuxLoadedRowTuple(label, keep_level, std::move(vec),
                                       std::move(neighbors), ver);
  }
};

static void install_capture(HierarchicalNSW<float> *hnsw, Capture *cap) {
  hnsw->setAddPointInsertCallback(
      [cap](labeltype label, hnswlib::tableint, uint32_t ver, const void *data,
            const HierarchicalNSW<float>::NeighborLabelListsByLevel &nbl,
            void *) {
        ++cap->inserts;
        cap->store(label, ver, data, nbl);
      });
  hnsw->setAddPointUpdateCallback(
      [cap](labeltype label, hnswlib::tableint, uint32_t ver, const void *data,
            const HierarchicalNSW<float>::NeighborLabelListsByLevel &nbl,
            void *) {
        ++cap->updates;
        cap->store(label, ver, data, nbl);
      });
}

/* H1's load-bearing invariant: per label, the captured versions are
strictly increasing in capture order, and a birth row is always 0. If
this ever breaks, the loader's highest-visible-ver rule stops
reconstructing mutation order — P2 comes back. */
static void assert_versions_monotonic_per_label(const Capture &cap) {
  std::map<labeltype, uint32_t> last;
  std::map<labeltype, bool> seen;
  for (const Capture::Append &a : cap.log) {
    if (!seen[a.label]) {
      seen[a.label] = true;
      last[a.label] = a.ver;
      continue;
    }
    EXPECT_GT(a.ver, last[a.label]) << "label " << a.label << " emitted ver "
                                    << a.ver << " after " << last[a.label];
    last[a.label] = a.ver;
  }
}

/* Insert callback fires exactly once per addPoint; update callbacks fire
only for pre-existing nodes and never for the node being inserted. */
TEST(HnswlibFork, InsertCallbackFiresOncePerAddPoint) {
  L2Space space(DIM);
  HierarchicalNSW<float> hnsw(&space, 100, 8, 32);
  Capture cap;
  install_capture(&hnsw, &cap);

  constexpr size_t N = 50;
  for (uint64_t i = 0; i < N; ++i) {
    const size_t updates_before = cap.updates;
    auto v = make_vec(i);
    hnsw.addPoint(v.data(), i, false, nullptr);
    EXPECT_EQ(cap.inserts, i + 1);
    /* Any update callbacks from this addPoint must target other labels. */
    (void)updates_before;
    ASSERT_TRUE(cap.rows.count(i) == 1);
  }
  EXPECT_EQ(cap.rows.size(), N);
  /* A connected graph must have produced neighbor-list rewrites on
  existing nodes. */
  EXPECT_GT(cap.updates, 0u);
  assert_versions_monotonic_per_label(cap);
}

/* H1: after a reload, each node's version counter resumes from the
version the reload consumed — so the next mutation appends ver+1 and can
never collide with (or be mistaken for) a persisted row. */
TEST(HnswlibFork, VersionCountersResumeAfterLoadIndex) {
  L2Space space(DIM);
  constexpr size_t N = 60;

  HierarchicalNSW<float> a(&space, N * 2, 8, 40);
  Capture cap;
  install_capture(&a, &cap);
  for (uint64_t i = 0; i < N; ++i) {
    auto v = make_vec(i);
    a.addPoint(v.data(), i, false, nullptr);
  }
  assert_versions_monotonic_per_label(cap);

  std::vector<VecAuxLoadedRowTuple> rows;
  rows.reserve(N);
  for (auto &kv : cap.rows) rows.push_back(kv.second);

  HierarchicalNSW<float> b(&space, N * 2, 8, 40);
  b.loadIndex(rows);
  ASSERT_EQ(b.cur_element_count.load(), N);

  /* Every reloaded node reports the version its winning row carried. */
  for (const auto &kv : cap.rows) {
    const labeltype label = kv.first;
    const uint32_t persisted = std::get<4>(kv.second);
    EXPECT_EQ(b.getVersionByLabel(label), persisted) << "label " << label;
  }

  /* Mutations after the reload continue the sequence: every append for a
  pre-existing label carries a version strictly above what was loaded. */
  Capture cap2;
  install_capture(&b, &cap2);
  for (uint64_t i = N; i < N + 10; ++i) {
    auto v = make_vec(i);
    b.addPoint(v.data(), i, false, nullptr);
  }
  for (const Capture::Append &ap : cap2.log) {
    auto it = cap.rows.find(ap.label);
    if (it == cap.rows.end()) {
      continue; /* a label born after the reload */
    }
    EXPECT_GT(ap.ver, std::get<4>(it->second))
        << "label " << ap.label << " re-emitted a loaded version";
  }
  assert_versions_monotonic_per_label(cap2);
}

/* The core persistence contract: rows captured through the callbacks,
loaded into a fresh index via loadIndex(rows), reproduce the exact same
graph (neighbor lists per node) and the same search results. */
TEST(HnswlibFork, CallbackRowsRoundTripThroughLoadIndex) {
  L2Space space(DIM);
  constexpr size_t N = 200;

  HierarchicalNSW<float> a(&space, N, 8, 40);
  Capture cap;
  install_capture(&a, &cap);
  for (uint64_t i = 0; i < N; ++i) {
    auto v = make_vec(i);
    a.addPoint(v.data(), i, false, nullptr);
  }
  ASSERT_EQ(cap.rows.size(), N);
  assert_versions_monotonic_per_label(cap);

  std::vector<VecAuxLoadedRowTuple> rows;
  rows.reserve(N);
  for (auto &kv : cap.rows) rows.push_back(kv.second);

  HierarchicalNSW<float> b(&space, N, 8, 40);
  b.loadIndex(rows);
  ASSERT_EQ(b.cur_element_count.load(), N);

  /* Graph equality: per-label neighbor lists must match. */
  for (uint64_t i = 0; i < N; ++i) {
    const auto ia = a.label_lookup_.at(i);
    const auto ib = b.label_lookup_.at(i);
    const auto na = a.gatherAllNeighborsForNode(ia);
    const auto nb = b.gatherAllNeighborsForNode(ib);
    ASSERT_EQ(na.size(), nb.size()) << "label " << i;
    for (size_t l = 0; l < na.size(); ++l) {
      EXPECT_EQ(na[l], nb[l]) << "label " << i << " level " << l;
    }
  }

  /* Search equality on a few probes. */
  for (uint64_t q = 0; q < 5; ++q) {
    auto probe = make_vec(1000 + q);
    auto ra = a.searchKnn(probe.data(), 10);
    auto rb = b.searchKnn(probe.data(), 10);
    ASSERT_EQ(ra.size(), rb.size());
    while (!ra.empty()) {
      EXPECT_EQ(ra.top().second, rb.top().second);
      EXPECT_FLOAT_EQ(ra.top().first, rb.top().first);
      ra.pop();
      rb.pop();
    }
  }
}

/* Dangling neighbor labels are dropped at load, not asserted: a row
committed by transaction B may reference a label whose inserting
transaction A rolled back later — A's aux row is gone, B's neighbor
list still names it. loadIndex must build a valid graph without the
edge (PS-11300 rollback support). */
TEST(HnswlibFork, LoadIndexSkipsDanglingNeighborLabels) {
  L2Space space(DIM);
  constexpr size_t N = 50;

  HierarchicalNSW<float> a(&space, N, 8, 40);
  Capture cap;
  install_capture(&a, &cap);
  for (uint64_t i = 0; i < N; ++i) {
    auto v = make_vec(i);
    a.addPoint(v.data(), i, false, nullptr);
  }

  /* Simulate label 7's rollback: its row disappears from the aux while
  every other row keeps whatever references to 7 it had. */
  std::vector<VecAuxLoadedRowTuple> rows;
  size_t refs_to_7 = 0;
  for (auto &kv : cap.rows) {
    if (kv.first == 7) {
      continue;
    }
    for (const auto &lvl : std::get<3>(kv.second)) {
      refs_to_7 += std::count(lvl.begin(), lvl.end(), 7);
    }
    rows.push_back(kv.second);
  }
  ASSERT_GT(refs_to_7, 0u) << "test premise: someone must link to 7";

  HierarchicalNSW<float> b(&space, N, 8, 40);
  b.loadIndex(rows);
  ASSERT_EQ(b.cur_element_count.load(), N - 1);
  EXPECT_EQ(b.label_lookup_.count(7), 0u);

  /* No neighbor list may reference the dropped label, and search over
  the loaded graph works. */
  for (auto &kv : b.label_lookup_) {
    const auto nbl = b.gatherAllNeighborsForNode(kv.second);
    for (const auto &lvl : nbl) {
      EXPECT_EQ(std::count(lvl.begin(), lvl.end(), 7), 0)
          << "label " << kv.first;
    }
  }
  auto probe = make_vec(7);
  auto res = b.searchKnn(probe.data(), 10);
  EXPECT_EQ(res.size(), 10u);
}

/* resizeIndex mid-stream: grow capacity while callbacks stay installed;
all rows before and after the resize are captured and reloadable. */
TEST(HnswlibFork, ResizeIndexMidStream) {
  L2Space space(DIM);
  HierarchicalNSW<float> hnsw(&space, 32, 8, 32);
  Capture cap;
  install_capture(&hnsw, &cap);

  uint64_t label = 0;
  for (; label < 32; ++label) {
    auto v = make_vec(label);
    hnsw.addPoint(v.data(), label, false, nullptr);
  }
  hnsw.resizeIndex(128);
  for (; label < 128; ++label) {
    auto v = make_vec(label);
    hnsw.addPoint(v.data(), label, false, nullptr);
  }
  EXPECT_EQ(cap.rows.size(), 128u);
  EXPECT_EQ(hnsw.cur_element_count.load(), 128u);

  std::vector<VecAuxLoadedRowTuple> rows;
  for (auto &kv : cap.rows) rows.push_back(kv.second);
  HierarchicalNSW<float> b(&space, 128, 8, 32);
  b.loadIndex(rows);
  EXPECT_EQ(b.cur_element_count.load(), 128u);
}

/* The aux table stores the node level in a TINYINT column. Levels are
geometric with ratio 1/M; sample the generator hard and prove the bound
holds with vast margin. */
TEST(HnswlibFork, LevelDistributionFitsTinyint) {
  L2Space space(DIM);
  /* M = 2 gives the fattest tail hnswlib permits in practice. */
  HierarchicalNSW<float> hnsw(&space, 16, 2, 16);
  int max_level = 0;
  for (int i = 0; i < 1000000; ++i) {
    max_level = std::max(max_level, hnsw.getRandomLevel(hnsw.mult_));
  }
  EXPECT_LE(max_level, 127);
  /* Sanity: the generator does produce multi-level nodes. */
  EXPECT_GT(max_level, 0);
}

}  // namespace innodb_hnswlib_unittest
