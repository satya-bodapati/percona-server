/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/**
  @file

  HnswVisitedSet, the visited set of SEARCH-LAYER, against
  ankerl::unordered_dense::set and std::unordered_set.

  The correctness test runs in any build. The timing test is skipped in debug
  builds; like hnsw_bench-t this target is not built by default:

    make hnsw_visited_set-t
    ./runtime_output_directory/hnsw_visited_set-t

  Three sets, each timed two ways, with the same logic for all of them:
    reserve  reserve(ef * Mmax) up front, as search_layer() does;
    grow     no reserve, the set grows in stages as it fills.

  A search is modelled on SEARCH-LAYER: about 2 * ef candidates are expanded
  and each has Mmax neighbours checked, so a search makes 2 * ef * Mmax
  insert-if-absent calls. They are drawn from a pool of distinct nodes, so
  some calls find a node already visited. Two pool sizes:
    full   ef * Mmax nodes, as many as reserve() plans for;
    short  ef * Mmax / 8 nodes, a search that visits far fewer nodes than
           reserve() planned for, which is where reserving up front costs.
  The sets only hash pointers; the nodes are never read.
*/

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <unordered_set>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "vector-common/hnsw.h"

namespace {

/** A graph node as the sets see it: only its address matters. */
struct alignas(64) FakeNode {
  char pad[64];
};

/** One insert-if-absent interface over the three sets. */
struct FlatSet {
  HnswVisitedSet<FakeNode> s;
  void reserve(size_t n) { s.reserve(n); }
  bool insert(FakeNode *p) { return s.insert(p); }
  size_t count(const FakeNode *p) const { return s.count(p); }
};

struct AnkerlSet {
  ankerl::unordered_dense::set<FakeNode *> s;
  void reserve(size_t n) { s.reserve(n); }
  bool insert(FakeNode *p) { return s.insert(p).second; }
  size_t count(FakeNode *p) const { return s.count(p); }
};

struct StdSet {
  std::unordered_set<FakeNode *> s;
  void reserve(size_t n) { s.reserve(n); }
  bool insert(FakeNode *p) { return s.insert(p).second; }
  size_t count(FakeNode *p) const { return s.count(p); }
};

/** Mmax on layer 0 for M = 16. */
constexpr size_t kMmax = 32;

/** Nodes the pointers are taken from: 128 MiB of 64-byte nodes. */
std::vector<FakeNode> &arena() {
  static std::vector<FakeNode> nodes(2 * 1024 * 1024);
  return nodes;
}

/**
  The pointers one search checks, in order: 2 * ef * Mmax draws from a pool
  of @p pool distinct nodes spread over the arena.
*/
std::vector<FakeNode *> search_sequence(size_t ef, size_t pool,
                                        std::mt19937_64 &rng) {
  std::vector<FakeNode> &nodes = arena();
  std::uniform_int_distribution<size_t> any(0, nodes.size() - 1);
  std::vector<FakeNode *> distinct(pool);
  for (FakeNode *&p : distinct) p = &nodes[any(rng)];
  std::uniform_int_distribution<size_t> pick(0, pool - 1);
  std::vector<FakeNode *> seq(2 * ef * kMmax);
  for (FakeNode *&p : seq) p = distinct[pick(rng)];
  return seq;
}

TEST(HnswVisitedSet, MatchesUnorderedSet) {
  std::mt19937_64 rng(42);
  // Sizes around the rehash thresholds, with and without reserve().
  for (const size_t ef : {1, 2, 10, 40, 200}) {
    for (const bool reserve : {false, true}) {
      const std::vector<FakeNode *> seq = search_sequence(ef, ef * 4, rng);
      FlatSet flat;
      StdSet ref;
      if (reserve) flat.reserve(ef * kMmax);
      for (FakeNode *p : seq) {
        ASSERT_EQ(flat.count(p), ref.count(p));
        ASSERT_EQ(flat.insert(p), ref.insert(p));
        ASSERT_EQ(flat.count(p), 1u);
      }
      EXPECT_FALSE(flat.s.empty());
      // A node never inserted is not found.
      FakeNode other;
      EXPECT_EQ(flat.count(&other), 0u);
    }
  }
  HnswVisitedSet<FakeNode> empty;
  FakeNode n;
  EXPECT_TRUE(empty.empty());
  EXPECT_EQ(empty.count(&n), 0u);
}

/** Nanoseconds per search for @p Set, timed @p reps times; the best is kept. */
template <typename Set>
double time_searches(const std::vector<std::vector<FakeNode *>> &seqs,
                     size_t searches, size_t reserve_n, uint64_t *sink) {
  double best = 1e300;
  for (int rep = 0; rep < 3; ++rep) {
    uint64_t found = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < searches; ++i) {
      Set set;  // search_layer() makes a new set per call
      if (reserve_n != 0) set.reserve(reserve_n);
      for (FakeNode *p : seqs[i % seqs.size()]) found += set.insert(p);
    }
    const double ns = std::chrono::duration<double, std::nano>(
                          std::chrono::steady_clock::now() - t0)
                          .count() /
                      searches;
    if (ns < best) best = ns;
    *sink += found;
  }
  return best;
}

TEST(HnswVisitedSetBenchmark, InsertIfAbsent) {
#ifndef NDEBUG
  GTEST_SKIP() << "Benchmarks skipped in debug builds "
                  "(configure with -DWITH_DEBUG=OFF for meaningful results)";
#endif
  std::mt19937_64 rng(7);
  uint64_t sink = 0;
  printf(
      "\n%-5s %-5s %7s %8s | %11s %11s | %11s %11s | %11s %11s  (ns per "
      "search)\n",
      "ef", "pool", "checks", "distinct", "flat+res", "flat grow", "ankerl+res",
      "ankerl grow", "std+res", "std grow");
  for (const size_t ef : {40, 200, 800}) {
    for (const bool full : {true, false}) {
      const size_t pool = full ? ef * kMmax : ef * kMmax / 8;
      // A few different searches, cycled, so no one pattern stays cached.
      std::vector<std::vector<FakeNode *>> seqs;
      for (int k = 0; k < 64; ++k)
        seqs.push_back(search_sequence(ef, pool, rng));
      size_t distinct = 0;
      {
        StdSet d;
        for (FakeNode *p : seqs[0]) distinct += d.insert(p);
      }
      // About 50 million checks per timed run.
      const size_t searches = 50'000'000 / seqs[0].size();
      const size_t plan = ef * kMmax;
      const double flat_r = time_searches<FlatSet>(seqs, searches, plan, &sink);
      const double flat_g = time_searches<FlatSet>(seqs, searches, 0, &sink);
      const double ank_r =
          time_searches<AnkerlSet>(seqs, searches, plan, &sink);
      const double ank_g = time_searches<AnkerlSet>(seqs, searches, 0, &sink);
      const double std_r = time_searches<StdSet>(seqs, searches, plan, &sink);
      const double std_g = time_searches<StdSet>(seqs, searches, 0, &sink);
      printf(
          "%-5zu %-5s %7zu %8zu | %11.0f %11.0f | %11.0f %11.0f | %11.0f "
          "%11.0f\n",
          ef, full ? "full" : "short", seqs[0].size(), distinct, flat_r, flat_g,
          ank_r, ank_g, std_r, std_g);
    }
  }
  printf("(sink %llu)\n", static_cast<unsigned long long>(sink));
  EXPECT_GT(sink, 0u);
}

}  // namespace
