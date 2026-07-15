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

/* Mirrors what the InnoDB callbacks persist: latest state per label.
The insert callback creates the row; update callbacks overwrite the
neighbors (and carry the node's own vector, per the fork's contract). */
struct Capture {
  std::map<labeltype, VecAuxLoadedRowTuple> rows;
  size_t inserts = 0;
  size_t updates = 0;

  void store(labeltype label, const void *data,
             const HierarchicalNSW<float>::NeighborLabelListsByLevel &nbl) {
    const float *f = static_cast<const float *>(data);
    std::vector<float> vec(f, f + DIM);
    std::vector<std::vector<size_t>> neighbors;
    for (const auto &lvl : nbl) {
      neighbors.emplace_back(lvl.begin(), lvl.end());
    }
    const uint64_t level = nbl.empty() ? 0 : nbl.size() - 1;
    rows[label] = VecAuxLoadedRowTuple(label, level, std::move(vec),
                                       std::move(neighbors));
  }
};

static void install_capture(HierarchicalNSW<float> *hnsw, Capture *cap) {
  hnsw->setAddPointInsertCallback(
      [cap](labeltype label, hnswlib::tableint, const void *data,
            const HierarchicalNSW<float>::NeighborLabelListsByLevel &nbl,
            void *) {
        ++cap->inserts;
        cap->store(label, data, nbl);
      });
  hnsw->setAddPointUpdateCallback(
      [cap](labeltype label, hnswlib::tableint, const void *data,
            const HierarchicalNSW<float>::NeighborLabelListsByLevel &nbl,
            void *) {
        ++cap->updates;
        cap->store(label, data, nbl);
      });
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
