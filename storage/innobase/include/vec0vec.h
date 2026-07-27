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

#pragma once

#include <string_view>
#include <variant>
#include "key_spec.h"
#include "lex_string.h"

namespace storage::innobase::vec {

bool validate_options(const Key_spec &index_def);

struct HnswParam {
  int M{25};
  int max_elements{10000};
  int ef_construction{200};
  std::string_view metric{"euclidean"};
};

/** TYPE spann construction parameters — none accepted yet (S2 adds
heads_pct etc.); the empty struct keys the variant. */
struct SpannParam {};

using VectorIndexParam = std::variant<std::monostate, HnswParam, SpannParam>;

bool parse_options(const Key_spec &index_def, VectorIndexParam &vip);

/** Parse the serialized construction-parameter string ("k=v,k=v", the
format produced at CREATE time and stored in the DD / KEY::
vector_construction_params) back into an HnswParam. Used on the table-
open path, where the Key_spec no longer exists. The string was
validated by parse_options at DDL time, so failure here indicates DD
corruption; an error is raised and true returned.
@param[in]  params  serialized parameters; str may be nullptr (no params)
@param[out] out     parsed parameters (defaults where keys are absent)
@return true on error */
bool parse_construction_params(const LEX_CSTRING &params, HnswParam &out);

}  // namespace storage::innobase::vec
