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

#include "vec0vec.h"

#include "vector-common/vector_distance.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <iterator>
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

namespace {
const char *alg_to_string(ha_key_alg alg) {
  switch (alg) {
    case HA_KEY_ALG_SE_SPECIFIC:
      ut_ad(0); /* the accepted algorithm; never named in an error */
      return "SE-SPECIFIC";
    case HA_KEY_ALG_BTREE:
      return "BTREE";
    case HA_KEY_ALG_RTREE:
      return "RTREE";
    case HA_KEY_ALG_HASH:
      return "HASH";
    case HA_KEY_ALG_FULLTEXT:
      return "FULLTEXT";
    case HA_KEY_ALG_VECTOR:
      return "VECTOR";
  }

  ut_ad(0); /* never nullptr: the caller passes this to my_error as %s */
  return "UNKNOWN";
}
}  // namespace

namespace storage::innobase::vec {

/* The shared implementation. Takes the two fields that actually matter -
the TYPE token and the WITH(...) list - so the same parse serves DDL,
where they arrive on a Key_spec, and table open, where they arrive on a
KEY. */
namespace {
/** Parse an integer value: the whole string, within [min, max].
@return false if it is not one */
bool parse_int(const LEX_CSTRING &value, int min, int max, int &out) {
  const auto *last = value.str + value.length;
  int val;
  const auto result = std::from_chars(value.str, last, val);
  if (result.ptr != last || result.ec != errc() || val < min || val > max)
    return false;
  out = val;
  return true;
}

/** Parse a metric name.
@return false if it is not one */
bool parse_metric(const LEX_CSTRING &value, vector_constants::Metric &out) {
  const auto *metric =
      vector_constants::metric_from_name({value.str, value.length});
  if (metric == nullptr) return false;
  out = *metric;
  return true;
}

/** An HNSW option: its name, and how its value is parsed, checked and
stored. */
struct Hnsw_option {
  const char *name;
  bool (*parse)(const LEX_CSTRING &value, HnswParam &param);
};

/* One entry per option: {"name", function that parses, checks and stores
its value}. */
constexpr Hnsw_option hnsw_options[] = {
    {"M",
     [](const LEX_CSTRING &value, HnswParam &param) {
       return parse_int(value, hnsw::min_M, hnsw::max_M, param.M);
     }},
    {"ef_construction",
     [](const LEX_CSTRING &value, HnswParam &param) {
       return parse_int(value, hnsw::min_ef_construction,
                        hnsw::max_ef_construction, param.ef_construction);
     }},
    {"metric",
     [](const LEX_CSTRING &value, HnswParam &param) {
       return parse_metric(value, param.metric);
     }},
};

/** Copy a string onto a MEM_ROOT as a LEX_CSTRING.
@return false on OOM */
bool dup_lex_cstring(MEM_ROOT *mem_root, std::string_view s,
                     LEX_CSTRING *out) {
  out->str = strmake_root(mem_root, s.data(), s.size());
  out->length = s.size();
  return out->str != nullptr;
}
}  // namespace

bool parse_options(LEX_CSTRING type, const Vector_index_params_YY &params,
                   VectorIndexParam &vip) {
  if (type.str == nullptr) {
    my_error(ER_NO_INDEX_TYPE, MYF(0), "");
    return true;
  }

  if (my_strcasecmp(system_charset_info, type.str, "HNSW") != 0) {
    my_error(ER_INDEX_TYPE_NOT_SUPPORTED, MYF(0), type.str, "vector");
    return true;
  }

  auto &hnsw_param = vip.emplace<HnswParam>();

  for (const auto *param = params.begin(); param != params.end(); ++param) {
    const auto &[key, value] = *param;
    /* Unknown name, e.g. (bogus = 1). */
    const auto *option =
        std::find_if(std::begin(hnsw_options), std::end(hnsw_options),
                     [&](const Hnsw_option &candidate) {
                       return my_strcasecmp(system_charset_info, key.str,
                                            candidate.name) == 0;
                     });
    if (option == std::end(hnsw_options)) {
      my_error(ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER, MYF(0), key.str);
      return true;
    }

    /* Name given twice, case-insensitively, e.g. (M = 8, m = 8). */
    const bool repeated =
        std::any_of(params.begin(), param, [&](const auto &earlier) {
          return my_strcasecmp(system_charset_info, earlier.first.str,
                               key.str) == 0;
        });
    if (repeated) {
      my_error(ER_DUPLICATE_INDEX_CONSTRUCTION_PARAMETER, MYF(0), key.str);
      return true;
    }

    /* Bad value, e.g. (M = abc), (M = 500) or (metric = nosuchmetric). */
    if (!option->parse(value, hnsw_param)) {
      my_error(ER_ILLEGAL_INDEX_CONSTRUCTION_PARAMETER_VALUE, MYF(0),
               value.str);
      return true;
    }
  }

  /* Set the distance function for the accepted metrics. EUCLIDEAN uses
  squared L2: the graph only compares distances, so skipping the square
  root does not change ordering. COSINE uses the cosine distance as-is. */
  switch (hnsw_param.metric) {
    case vector_constants::Metric::kEuclidean:
      hnsw_param.dist = &vector_distance_euclidean_squared;
      break;
    case vector_constants::Metric::kCosine:
      hnsw_param.dist = &vector_distance_cosine;
      break;
    default:
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "HNSW indexes on anything but the EUCLIDEAN or COSINE metric");
      return true;
  }

  return false;
}

namespace {
/** Check the SHAPE of a vector index as the user wrote it: a non-prefixed
column and an SE-specific algorithm.
@return true on error, reported through my_error() */
bool check_shape(const Key_spec &index_def) {
  /* Column count is checked later by the server, not here. */
  ut_ad(!index_def.columns.empty());

  if (index_def.columns[0]->get_prefix_length() != 0) {
    my_error(ER_WRONG_SUB_KEY, MYF(0));
    return true;
  }

  if (index_def.key_create_info.algorithm != HA_KEY_ALG_SE_SPECIFIC) {
    my_error(ER_INDEX_TYPE_NOT_SUPPORTED, MYF(0),
             alg_to_string(index_def.key_create_info.algorithm), "vector");
    return true;
  }

  return false;
}
}  // namespace

/* DDL-time: validate the shape the user wrote, then parse. */
bool parse_options(const Key_spec &index_def, VectorIndexParam &vip) {
  if (index_def.type != KEYTYPE_VECTOR) return false;

  if (check_shape(index_def)) return true;

  return parse_options(index_def.key_create_info.vector_index_type,
                       index_def.key_create_info.vector_index_params, vip);
}

/* Open-time: the definition came back from the DD, so its shape was
already validated at DDL time. Parse only. */
bool parse_options(const KEY &key, VectorIndexParam &vip) {
  return parse_options(key.vector_index_type, key.vector_index_params, vip);
}

vector_constants::Metric metric_from_key(const KEY &key) {
  for (const auto &[opt, value] : key.vector_index_params) {
    if (my_strcasecmp(system_charset_info, opt.str, "metric") != 0) continue;
    if (value.str == nullptr) return vector_constants::Metric::kEuclidean;
    const auto *m = vector_constants::metric_from_name(
        std::string_view(value.str, value.length));
    if (m == nullptr) return vector_constants::Metric::kEuclidean;
    return *m;
  }
  return vector_constants::Metric::kEuclidean;
}

/* DDL-time: the one place the defaults turn into stored values. */
bool resolve_options(MEM_ROOT *mem_root, KEY *key) {
  VectorIndexParam vip;
  if (parse_options(*key, vip)) return true;
  const auto &hp = std::get<HnswParam>(vip);

  const std::string m = std::to_string(hp.M);
  const std::string efc = std::to_string(hp.ef_construction);
  const std::pair<std::string_view, std::string_view> resolved[] = {
      {"M", m},
      {"metric", vector_constants::metric_name(hp.metric)},
      {"ef_construction", efc},
  };

  /* A new list rather than an edit in place: the old one may be shared with
  the Key_spec, which a prepared statement executes again. */
  Vector_index_params_YY params;
  params.init(mem_root);
  for (const auto &[k, v] : resolved) {
    std::pair<LEX_CSTRING, LEX_CSTRING> param;
    if (!dup_lex_cstring(mem_root, k, &param.first) ||
        !dup_lex_cstring(mem_root, v, &param.second) ||
        params.push_back(param)) {
      return true; /* OOM, already reported by the mem_root */
    }
  }
  key->vector_index_params = params;
  return false;
}

bool prepare_options(MEM_ROOT *mem_root, const Key_spec &index_def,
                     KEY *key) {
  /* The KEY carries the Key_spec's TYPE and WITH(...) list, so resolving
  it checks the contents. */
  return check_shape(index_def) || resolve_options(mem_root, key);
}

}  // namespace storage::innobase::vec
