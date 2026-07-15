/*****************************************************************************

Copyright (c) 2025, Percona Inc.

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

#include "my_base.h"
#include "mysqld_error.h"
#include "sql/sql_alter.h"
#include "sql/sql_lex.h"
#include "storage/innobase/include/vec0vec.h"
#include "unittest/gunit/parsertest.h"
#include "unittest/gunit/test_utils.h"

namespace innodb_vec0vec_unittest {

using namespace storage::innobase::vec;
using my_testing::Server_initializer;

class Vec0VecTest : public ParserTest {
 protected:
  bool parse(const char *query, int expected_error = 0) {
    ParserTest::parse(query);

    const Key_spec *ks = find_vector_key();
    if (ks == nullptr) {
      ADD_FAILURE() << "No VECTOR key found in key_list";
      return true;
    }

    Server_initializer::set_expected_error(expected_error);
    bool err = parse_options(*ks, m_vip);
    Server_initializer::set_expected_error(0);
    return err;
  }

  const Key_spec *find_vector_key() {
    const Alter_info *alter_info = thd()->lex->alter_info;
    if (alter_info == nullptr) return nullptr;
    for (const Key_spec *ks : alter_info->key_list) {
      if (ks->type == KEYTYPE_VECTOR) return ks;
    }
    return nullptr;
  }

  VectorIndexParam m_vip;
};

TEST_F(Vec0VecTest, HnswWithM) {
  EXPECT_FALSE(
      parse("CREATE TABLE t1 ("
            "  id BIGINT UNSIGNED PRIMARY KEY,"
            "  v1 VECTOR(128),"
            "  VECTOR KEY(v1) TYPE hnsw WITH (M = 16)"
            ")"));
  ASSERT_TRUE(std::holds_alternative<HnswParam>(m_vip));
  EXPECT_EQ(16, std::get<HnswParam>(m_vip).M);
}

TEST_F(Vec0VecTest, HnswMetricEuclidean) {
  EXPECT_FALSE(
      parse("CREATE TABLE t1 ("
            "  id BIGINT UNSIGNED PRIMARY KEY,"
            "  v1 VECTOR(128),"
            "  VECTOR KEY(v1) TYPE hnsw WITH (metric = euclidean)"
            ")"));
  ASSERT_TRUE(std::holds_alternative<HnswParam>(m_vip));
  EXPECT_EQ("euclidean", std::get<HnswParam>(m_vip).metric);
}

TEST_F(Vec0VecTest, NonSeSpecificAlgorithm) {
  ParserTest::parse(
      "CREATE TABLE t1 ("
      "  id BIGINT UNSIGNED PRIMARY KEY,"
      "  v1 VECTOR(128),"
      "  KEY(id) USING BTREE"
      ")");
  const Alter_info *alter_info = thd()->lex->alter_info;
  // Find the BTREE key (not PRIMARY, not VECTOR)
  const Key_spec *ks = nullptr;
  for (const Key_spec *k : alter_info->key_list) {
    if (k->type == KEYTYPE_MULTIPLE) {
      ks = k;
      break;
    }
  }
  ASSERT_NE(nullptr, ks);
  EXPECT_FALSE(parse_options(*ks, m_vip));
}

TEST_F(Vec0VecTest, HnswEfConstruction) {
  EXPECT_FALSE(
      parse("CREATE TABLE t1 ("
            "  id BIGINT UNSIGNED PRIMARY KEY,"
            "  v1 VECTOR(128),"
            "  VECTOR KEY(v1) TYPE hnsw WITH (M = 16, ef_construction = 100)"
            ")"));
  ASSERT_TRUE(std::holds_alternative<HnswParam>(m_vip));
  EXPECT_EQ(16, std::get<HnswParam>(m_vip).M);
  EXPECT_EQ(100, std::get<HnswParam>(m_vip).ef_construction);
}

TEST_F(Vec0VecTest, HnswEfConstructionRejectsNonNumeric) {
  /* Note: the bad value must not be a lexer keyword (e.g. `fast`), or
  the statement dies as a syntax error before reaching parse_options. */
  EXPECT_TRUE(
      parse("CREATE TABLE t1 ("
            "  id BIGINT UNSIGNED PRIMARY KEY,"
            "  v1 VECTOR(128),"
            "  VECTOR KEY(v1) TYPE hnsw WITH (ef_construction = bogusval)"
            ")",
            ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER_VALUE));
}

/* The serialized "k=v,k=v" form parsed on the table-open path must
agree with the Key_spec form parsed at DDL time. */
TEST_F(Vec0VecTest, ConstructionParamsStringForm) {
  HnswParam p;

  EXPECT_FALSE(parse_construction_params({nullptr, 0}, p));
  EXPECT_EQ(25, p.M); /* defaults */
  EXPECT_EQ(200, p.ef_construction);

  const char *s1 = "M=16,ef_construction=100";
  EXPECT_FALSE(parse_construction_params({s1, strlen(s1)}, p));
  EXPECT_EQ(16, p.M);
  EXPECT_EQ(100, p.ef_construction);

  const char *s2 = "metric=euclidean";
  EXPECT_FALSE(parse_construction_params({s2, strlen(s2)}, p));
  EXPECT_EQ("euclidean", p.metric);
  EXPECT_EQ(25, p.M); /* reset to default per call */

  const char *bad1 = "M=abc";
  Server_initializer::set_expected_error(
      ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER_VALUE);
  EXPECT_TRUE(parse_construction_params({bad1, strlen(bad1)}, p));

  const char *bad2 = "bogus=1";
  Server_initializer::set_expected_error(
      ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER);
  EXPECT_TRUE(parse_construction_params({bad2, strlen(bad2)}, p));

  const char *bad3 = "M16"; /* no '=' */
  Server_initializer::set_expected_error(
      ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER);
  EXPECT_TRUE(parse_construction_params({bad3, strlen(bad3)}, p));
  Server_initializer::set_expected_error(0);
}

}  // namespace innodb_vec0vec_unittest
