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

/** @file vec/vec0index.cc
The vector-index TYPE registry: one static table binding each TYPE
token to its implementation singleton. */

#include "vec0index.h"

#include "univ.i"

#include "m_string.h"

namespace {

/** TYPE hnsw. */
class Vec_hnsw_index final : public Vector_index {
 public:
  [[nodiscard]] Vec_index_type type() const override {
    return Vec_index_type::HNSW;
  }
};

const Vec_hnsw_index vec_hnsw_singleton;

/** The TYPE registry (SPANN R3), indexed by Vec_index_type. Adding an
index TYPE = one enum value + one row here (S1 adds
{"spann", &vec_spann_singleton}). */
struct Vec_type_entry {
  const char *token;
  const Vector_index *impl;
};

const Vec_type_entry vec_type_registry[] = {
    /* order must match Vec_index_type */
    {"hnsw", &vec_hnsw_singleton},
};

}  // namespace

const Vector_index *vec_index_by_name(const char *token, size_t len) {
  if (token == nullptr || len == 0) {
    return nullptr;
  }
  for (const Vec_type_entry &e : vec_type_registry) {
    if (strlen(e.token) == len &&
        native_strncasecmp(e.token, token, len) == 0) {
      return e.impl;
    }
  }
  return nullptr;
}

const Vector_index *vec_index_by_enum(Vec_index_type type) {
  const auto i = static_cast<size_t>(type);
  ut_a(i < UT_ARR_SIZE(vec_type_registry));
  const Vector_index *impl = vec_type_registry[i].impl;
  ut_ad(impl->type() == type);
  return impl;
}
