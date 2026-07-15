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

#include "vec0vec.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <string>
#include <variant>

// ut0ut.h isn't self-contained.
#include "handler.h"
#include "lex_string.h"
#include "my_base.h"
#include "mysql/strings/m_ctype.h"
#include "mysqld_cs.h"
#include "ut0mem.h"
#include "ut0test.h"
#include "ut0ut.h"

#include <my_rapidjson_size_t.h>
#include <rapidjson/document.h>
#include <rapidjson/schema.h>
#include <rapidjson/stringbuffer.h>

#include "key_spec.h"
#include "my_sys.h"
#include "mysqld_error.h"

using namespace std;

namespace storage::innobase::vec {

namespace {

/** Apply one construction parameter to an HnswParam. Shared between the
Key_spec path (DDL validation) and the serialized-string path (table
open). `key`/`value` are NUL-terminated.
@return true on error (my_error raised) */
bool apply_hnsw_param(const char *key, const char *value, HnswParam &out) {
  const bool is_m = my_strcasecmp(system_charset_info, key, "M") == 0;
  const bool is_efc =
      !is_m &&
      my_strcasecmp(system_charset_info, key, "ef_construction") == 0;

  if (is_m || is_efc) {
    const size_t len = strlen(value);
    if (len == 0 || !std::all_of(value, value + len, [](unsigned char c) {
          return std::isdigit(c);
        })) {
      my_error(ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER_VALUE, MYF(0), value);
      return true;
    }
    (is_m ? out.M : out.ef_construction) = std::atoi(value);
    return false;
  }

  if (my_strcasecmp(system_charset_info, key, "metric") == 0) {
    if (my_strcasecmp(system_charset_info, value, "euclidean") == 0) {
      out.metric = "euclidean";
      return false;
    }
    my_error(ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER_VALUE, MYF(0), value);
    return true;
  }

  my_error(ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER, MYF(0), key);
  return true;
}

}  // namespace

bool validate_options(const Key_spec &index_def) {
  VectorIndexParam vip;
  return parse_options(index_def, vip);
}

bool parse_options(const Key_spec &index_def, VectorIndexParam &vip) {
  if (index_def.key_create_info.algorithm != HA_KEY_ALG_SE_SPECIFIC) {
    if (index_def.type == KEYTYPE_VECTOR) {
      my_error(ER_NO_INDEX_TYPE, MYF(0), "");
      return true;
    }
    return false;
  }

  if (index_def.key_create_info.vector_index_type.str == nullptr) {
    my_error(ER_NO_INDEX_TYPE, MYF(0), "");
    return true;
  }
  if (my_strcasecmp(system_charset_info,
                    index_def.key_create_info.vector_index_type.str,
                    "HNSW") == 0) {
    vip = HnswParam();
    auto &hnsw_param = std::get<HnswParam>(vip);
    for (const IndexConstructionParam &p : index_def.construction_params) {
      if (apply_hnsw_param(p.key.str, p.value.str, hnsw_param)) {
        return true;
      }
    }
    return false;
  }
  my_error(ER_INDEX_TYPE_NOT_SUPPORTED_FOR_VECTOR_INDEX, MYF(0),
           index_def.key_create_info.vector_index_type.str);
  return true;
}

bool parse_construction_params(const LEX_CSTRING &params, HnswParam &out) {
  out = HnswParam();

  if (params.str == nullptr || params.length == 0) {
    return false; /* no WITH() clause — defaults apply */
  }

  /* Format: "k=v,k=v" (produced at sql_table.cc key_info fill; no
  whitespace). Tokenize into NUL-terminated copies for the shared
  helper. */
  const std::string s(params.str, params.length);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t comma = s.find(',', pos);
    if (comma == std::string::npos) comma = s.size();
    const size_t eq = s.find('=', pos);
    if (eq == std::string::npos || eq >= comma || eq == pos) {
      my_error(ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER, MYF(0),
               s.substr(pos, comma - pos).c_str());
      return true;
    }
    const std::string key = s.substr(pos, eq - pos);
    const std::string value = s.substr(eq + 1, comma - eq - 1);
    if (apply_hnsw_param(key.c_str(), value.c_str(), out)) {
      return true;
    }
    pos = comma + 1;
  }
  return false;
}

}  // namespace storage::innobase::vec
