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

#include <gtest/gtest.h>

#include "storage/innobase/include/vec0hnsw.h"

namespace innodb_vec0hnsw_unittest {

/* Vec_persistor::load_node_cb's top guard:

    if (ctx->err != DB_SUCCESS &&
        (ctx->loading_entry_point || ctx->err != DB_RECORD_NOT_FOUND)) {
      return false;
    }

has no SQL-reachable trigger today: the only site that can leave
DB_RECORD_NOT_FOUND in ctx->err across a *later* load_node_cb call is the
cold-start entry-point load, and that load never makes a second call in the
same Vec_ctx. So the guard is currently a no-op on every real path - it is
defensive/symmetry code (see the comment on load_node_cb), kept so the guard
mirrors the assignment it protects if a second caller with this shape is
ever added. These tests pin its behaviour directly, since no MTR test can
observe it.

load_node_cb is a member template on Hnsw purely so LoadNodeHandle can be
nested in the real graph type - substituting MockHnsw below still calls the
real, unmodified Vec_persistor::load_node_cb and vec_persist_load_node()
(vec0hnsw.h); only the graph itself, a supporting piece, is stubbed out. The
"proceed past guard" cases below use MockHnsw::load_node_id() returning 0 to
make that clear: vec_persist_load_node's very first statement is
`ut_ad(id != 0)`, so if - and only if - the guard let control flow reach
that real function, the process aborts right there, before anything needs a
real aux table. The "short-circuit" cases never call load_node_id at all
(checked directly), so they run in-process. */

/** Bare-bones Hnsw stand-in. Every method the real vec_persist_load_node()
can call along its success path has to exist for the template to
instantiate, even though the "proceed past guard" test cases below never
reach them (they abort at vec_persist_load_node's own id==0 check first). */
struct MockHnsw {
  using LoadNodeHandle = void *;

  mutable bool load_node_id_called = false;

  uint64_t load_node_id(LoadNodeHandle) const {
    load_node_id_called = true;
    /* 0 is the id vec_persist_load_node treats as aux corruption
    (vec0hnsw.h: "Record 0 is the metadata record, never a node") and
    asserts on in a debug build. Returning it here is the probe: reaching
    that assert proves the guard let this call through for real, without
    needing a real aux table to read a row from. */
    return 0;
  }
  void load_set_layer(LoadNodeHandle, uint8_t) {}
  void load_set_vec(LoadNodeHandle, const char *) {}
  void load_set_base_pk(LoadNodeHandle, uint64_t) {}
  template <typename Range>
  void load_node_neighbors(LoadNodeHandle, Range) {}
};

class LoadNodeCbGuardTest : public ::testing::Test {
 protected:
  Vec_ctx ctx;
  MockHnsw hnsw;
  Vec_persistor persistor;
};

/* Split into its own *DeathTest suite, matching the codebase convention
(unittest/gunit/hnsw-t.cc): gtest runs *DeathTest suites first, before any
other test has spawned threads a fork-based death test would not carry
over. */
using LoadNodeCbGuardDeathTest = LoadNodeCbGuardTest;

#ifndef NDEBUG

/* (a) ctx->err == DB_SUCCESS: the guard's own condition is false
(ctx->err != DB_SUCCESS fails), so it must not short-circuit. Only pins
behavior - a success ctx->err takes this path under the old unconditional
"if (ctx->err != DB_SUCCESS) return false;" guard too. */
TEST_F(LoadNodeCbGuardDeathTest, SuccessDoesNotShortCircuit) {
  ctx.err = DB_SUCCESS;
  ctx.loading_entry_point = false;
  EXPECT_DEATH_IF_SUPPORTED(persistor.load_node_cb(&ctx, hnsw, nullptr), "");
}

/* (b) DB_RECORD_NOT_FOUND left over from a hypothetical prior call, NOT
loading the entry point: this is the exact case PR#18/PR#14 exist to keep
working - a stale "not found" from an earlier ordinary (non-entry-point)
load must not stop this call from trying its own row. This is the only one
of the four cases that actually discriminates the new guard from the old
unconditional "if (ctx->err != DB_SUCCESS) return false;": under the old
guard this ctx->err alone would short-circuit and the death below would not
happen, failing this test. */
TEST_F(LoadNodeCbGuardDeathTest,
       StaleNotFoundWithoutEntryPointDoesNotShortCircuit) {
  ctx.err = DB_RECORD_NOT_FOUND;
  ctx.loading_entry_point = false;
  EXPECT_DEATH_IF_SUPPORTED(persistor.load_node_cb(&ctx, hnsw, nullptr), "");
}

#endif  // NDEBUG

/* (c) Same stale DB_RECORD_NOT_FOUND, but this time loading the entry
point: that is the fatal case, so the guard must return false immediately
without ever touching the graph. Also true under the old unconditional
guard - only pins behavior. */
TEST_F(LoadNodeCbGuardTest, StaleNotFoundWithEntryPointShortCircuits) {
  ctx.err = DB_RECORD_NOT_FOUND;
  ctx.loading_entry_point = true;

  EXPECT_FALSE(persistor.load_node_cb(&ctx, hnsw, nullptr));
  EXPECT_FALSE(hnsw.load_node_id_called);
  /* The guard must not touch ctx->err - it only ever reads it here. */
  EXPECT_EQ(DB_RECORD_NOT_FOUND, ctx.err);
}

/* (d) Any other pre-existing failure is never expected, so it stays fatal
regardless of loading_entry_point. Also true under the old unconditional
guard - only pins behavior. */
TEST_F(LoadNodeCbGuardTest, OtherErrorShortCircuitsRegardlessOfEntryPoint) {
  for (bool loading_entry_point : {false, true}) {
    ctx = Vec_ctx{};
    ctx.err = DB_CORRUPTION;
    ctx.loading_entry_point = loading_entry_point;
    hnsw.load_node_id_called = false;

    EXPECT_FALSE(persistor.load_node_cb(&ctx, hnsw, nullptr))
        << "loading_entry_point=" << loading_entry_point;
    EXPECT_FALSE(hnsw.load_node_id_called)
        << "loading_entry_point=" << loading_entry_point;
    EXPECT_EQ(DB_CORRUPTION, ctx.err)
        << "loading_entry_point=" << loading_entry_point;
  }
}

}  // namespace innodb_vec0hnsw_unittest
