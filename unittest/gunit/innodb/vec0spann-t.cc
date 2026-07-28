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

/** @file unittest/gunit/innodb/vec0spann-t.cc
Tests for the SPANN closure-assignment rule (vec_spann_select_heads,
include/vec0spann.h): the epsilon closure bound, the RNG occlusion
rule, the replication cap, and the geometry end-to-end against the
same hnswlib L2 kernel the build pass uses. */

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <vector>

#include "extra/hnswlib/hnswlib.h"

#include "vec0spann.h"

namespace innodb_vec0spann_unittest {

/** head_dist functor over an explicit distance matrix. */
class Matrix_dist {
 public:
  void set(uint64_t a, uint64_t b, float d) {
    m_d[{a, b}] = d;
    m_d[{b, a}] = d;
  }
  float operator()(uint64_t a, uint64_t b) const { return m_d.at({a, b}); }

 private:
  std::map<std::pair<uint64_t, uint64_t>, float> m_d;
};

TEST(VecSpannSelectHeads, NearestAlwaysSelectedAndFirst) {
  Matrix_dist hd;
  std::vector<uint64_t> out{99}; /* must be replaced, not appended */

  vec_spann_select_heads({{1.0f, 7}}, 0.25f, 8, hd, &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 7u);
}

TEST(VecSpannSelectHeads, EmptyCandidatesSelectNothing) {
  Matrix_dist hd;
  std::vector<uint64_t> out{99};
  vec_spann_select_heads({}, 0.25f, 8, hd, &out);
  EXPECT_TRUE(out.empty());
}

TEST(VecSpannSelectHeads, EpsilonBoundsTheClosure) {
  /* limit = (1 + 0.25) * 1.0 = 1.25: h2 (1.2) qualifies, h3 (1.3)
  does not — and the loop may stop there because candidates are
  sorted. Heads far apart, so the RNG rule never interferes. */
  Matrix_dist hd;
  hd.set(1, 2, 100.0f);
  hd.set(1, 3, 100.0f);
  hd.set(2, 3, 100.0f);

  std::vector<uint64_t> out;
  vec_spann_select_heads({{1.0f, 1}, {1.2f, 2}, {1.3f, 3}}, 0.25f, 8, hd, &out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], 1u);
  EXPECT_EQ(out[1], 2u);
}

TEST(VecSpannSelectHeads, RngRuleOccludesHeadBehindSelected) {
  /* h2 sits behind h1 as seen from x: head_dist(h1, h2) < dist(x, h2)
  — a copy in h2's list is redundant, h1's list covers this boundary.
  Same distances with the heads far apart must select h2. */
  Matrix_dist behind;
  behind.set(1, 2, 1.0f);
  std::vector<uint64_t> out;
  vec_spann_select_heads({{2.0f, 1}, {2.2f, 2}}, 0.5f, 8, behind, &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 1u);

  Matrix_dist apart;
  apart.set(1, 2, 10.0f);
  vec_spann_select_heads({{2.0f, 1}, {2.2f, 2}}, 0.5f, 8, apart, &out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1], 2u);
}

TEST(VecSpannSelectHeads, RngComparesAgainstEverySelectedHead) {
  /* h3 clears the nearest head (h1) but is occluded by the SECOND
  selected head (h2): the rule must test all selected, not just the
  first. */
  Matrix_dist hd;
  hd.set(1, 2, 10.0f); /* h2 not occluded by h1 */
  hd.set(1, 3, 10.0f); /* h3 not occluded by h1 ... */
  hd.set(2, 3, 1.0f);  /* ... but occluded by h2 */

  std::vector<uint64_t> out;
  vec_spann_select_heads({{2.0f, 1}, {2.1f, 2}, {2.2f, 3}}, 0.5f, 8, hd, &out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], 1u);
  EXPECT_EQ(out[1], 2u);
}

TEST(VecSpannSelectHeads, MaxCopiesCapsSelection) {
  Matrix_dist hd;
  for (uint64_t a = 1; a <= 4; ++a) {
    for (uint64_t b = a + 1; b <= 4; ++b) {
      hd.set(a, b, 100.0f);
    }
  }
  std::vector<uint64_t> out;
  const std::vector<vec_spann_head_cand_t> cands = {
      {1.0f, 1}, {1.01f, 2}, {1.02f, 3}, {1.03f, 4}};
  vec_spann_select_heads(cands, 1.0f, 2, hd, &out);
  EXPECT_EQ(out.size(), 2u);
  vec_spann_select_heads(cands, 1.0f, 0, hd, &out);
  EXPECT_TRUE(out.empty());
}

TEST(VecSpannSelectHeads, ZeroDistanceNearestAdmitsOnlyTies) {
  /* A head's own vector: dist 0 to itself, so limit = 0 — only exact
  ties may replicate. */
  Matrix_dist hd;
  hd.set(1, 2, 4.0f);
  std::vector<uint64_t> out;
  vec_spann_select_heads({{0.0f, 1}, {4.0f, 2}}, 0.25f, 8, hd, &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 1u);
}

/** End-to-end on real geometry with the same L2 kernel the build pass
uses (squared L2): heads on a line at 0, 10, 11; the vector at 10.5 is
a boundary point between two close heads and must replicate to both;
the far head is epsilon-excluded. A vector at 0.4 belongs to head A
alone. */
TEST(VecSpannSelectHeads, L2GeometryClosure) {
  hnswlib::L2Space space(2);
  const auto dist = space.get_dist_func();
  void *param = space.get_dist_func_param();

  const std::map<uint64_t, std::vector<float>> heads = {
      {1, {0.0f, 0.0f}}, {2, {10.0f, 0.0f}}, {3, {11.0f, 0.0f}}};

  const auto head_dist = [&](uint64_t a, uint64_t b) {
    return dist(heads.at(a).data(), heads.at(b).data(), param);
  };

  const auto cands_for = [&](const std::vector<float> &x) {
    std::vector<vec_spann_head_cand_t> cands;
    for (const auto &h : heads) {
      cands.push_back({dist(x.data(), h.second.data(), param), h.first});
    }
    std::sort(cands.begin(), cands.end(),
              [](const vec_spann_head_cand_t &a,
                 const vec_spann_head_cand_t &b) { return a.dist < b.dist; });
    return cands;
  };

  std::vector<uint64_t> out;

  /* Midpoint of heads 2 and 3: both selected (RNG: the head pair is
  1.0 apart in squared L2, farther than the 0.25 to each), head 1
  epsilon-excluded. */
  vec_spann_select_heads(cands_for({10.5f, 0.0f}), VEC_SPANN_CLOSURE_EPS,
                         VEC_SPANN_CLOSURE_MAX, head_dist, &out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], 2u);
  EXPECT_EQ(out[1], 3u);

  /* Interior point: single assignment. */
  vec_spann_select_heads(cands_for({0.4f, 0.0f}), VEC_SPANN_CLOSURE_EPS,
                         VEC_SPANN_CLOSURE_MAX, head_dist, &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 1u);
}

/* ------------------------------------------------------------------
vec_spann_kmeans2 (L1 split): deterministic farthest-point init +
Lloyd's rounds, on the same L2 kernel the split job uses. */

static float l2sq(const float *a, const float *b, uint32_t dims) {
  float s = 0.0f;
  for (uint32_t d = 0; d < dims; ++d) {
    const float t = a[d] - b[d];
    s += t * t;
  }
  return s;
}

TEST(VecSpannKmeans2, SeparatesTwoClusters) {
  /* Two tight clusters on a line: centroids must land near 1 and 11,
  and every point must be closer to its own centroid. */
  const std::vector<std::vector<float>> data = {{0, 0},  {1, 0},  {2, 0},
                                                {10, 0}, {11, 0}, {12, 0}};
  std::vector<const float *> pts;
  for (const auto &v : data) pts.push_back(v.data());

  const auto dist = [](const float *a, const float *b) {
    return l2sq(a, b, 2);
  };

  std::vector<float> c1, c2;
  ASSERT_TRUE(vec_spann_kmeans2(pts, 2, dist, &c1, &c2));

  /* One centroid per cluster (order unspecified). */
  const float lo = std::min(c1[0], c2[0]);
  const float hi = std::max(c1[0], c2[0]);
  EXPECT_NEAR(lo, 1.0f, 0.001f);
  EXPECT_NEAR(hi, 11.0f, 0.001f);

  for (const auto &v : data) {
    const bool left = dist(v.data(), c1.data()) < dist(v.data(), c2.data())
                          ? c1[0] < c2[0]
                          : c2[0] < c1[0];
    EXPECT_EQ(left, v[0] < 5.0f);
  }
}

TEST(VecSpannKmeans2, DeterministicAcrossCalls) {
  const std::vector<std::vector<float>> data = {{3, 1}, {0, 0}, {9, 4}, {1, 1},
                                                {8, 5}, {2, 0}, {10, 5}};
  std::vector<const float *> pts;
  for (const auto &v : data) pts.push_back(v.data());
  const auto dist = [](const float *a, const float *b) {
    return l2sq(a, b, 2);
  };

  std::vector<float> a1, a2, b1, b2;
  ASSERT_TRUE(vec_spann_kmeans2(pts, 2, dist, &a1, &a2));
  ASSERT_TRUE(vec_spann_kmeans2(pts, 2, dist, &b1, &b2));
  EXPECT_EQ(a1, b1);
  EXPECT_EQ(a2, b2);
}

TEST(VecSpannKmeans2, RefusesDegenerateInputs) {
  const std::vector<float> p = {1, 2};
  const auto dist = [](const float *a, const float *b) {
    return l2sq(a, b, 2);
  };
  std::vector<float> c1, c2;

  /* One point. */
  EXPECT_FALSE(vec_spann_kmeans2({p.data()}, 2, dist, &c1, &c2));
  /* All identical: the halves would coincide and split nothing. */
  EXPECT_FALSE(
      vec_spann_kmeans2({p.data(), p.data(), p.data()}, 2, dist, &c1, &c2));
}

}  // namespace innodb_vec0spann_unittest
