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

/** @file include/vec0index.h
The vector-index TYPE registry.

A vector index TYPE is an identity (Vec_index_type) plus an
implementation singleton (Vector_index); the registry binds the two to
the string token SQL uses. Runtime operations are added to the
interface by the commits that implement them. */

#ifndef vec0index_h
#define vec0index_h

#include <cstddef>
#include <cstdint>

/** The registered vector index TYPEs. The enum value is the identity
used everywhere the type is already known (registry indexing, runtime
dispatch, future switch()es); the string token exists only at the
boundaries where SQL hands us text — DDL validation and the
KEY::vector_index_type reloaded from the DD (SPANN R3). */
enum class Vec_index_type : uint8_t {
  HNSW = 0,
  /* SPANN = 1 — added by commit S1 */
};

/** Per-TYPE vector-index implementations: STATELESS singletons. */
class Vector_index {
 public:
  virtual ~Vector_index() = default;

  /** @return this implementation's registered TYPE */
  [[nodiscard]] virtual Vec_index_type type() const = 0;
};

/** Look up the implementation registered under a TYPE token, e.g. the
"hnsw" of CREATE ... VECTOR KEY (v) TYPE hnsw (case-insensitive; the
token is not necessarily NUL-terminated). Registering a new TYPE is
one entry in vec_type_registry[] (vec0index.cc); rejecting unknown
TYPEs at CREATE/ALTER is what keeps them out of every engine path.
@return the implementation, or nullptr for an unknown TYPE */
[[nodiscard]] const Vector_index *vec_index_by_name(const char *token,
                                                    size_t len);

/** The implementation for a known TYPE — O(1), never nullptr. */
[[nodiscard]] const Vector_index *vec_index_by_enum(Vec_index_type type);

#endif /* vec0index_h */
