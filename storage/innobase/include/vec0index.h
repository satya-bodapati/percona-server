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
The vector-index type seam (SPANN plan, commit R1).

One abstract interface over the per-type runtime operations of a vector
index, so a second index TYPE (spann) can be added without touching the
HNSW implementation or the call sites again. Deliberately MINIMAL:
only the operations that exist today, no speculative hooks — methods
are added when a second implementation demands them.

What is BEHIND the seam (type-specific runtime):
  open / load / insert / remove / refresh_row_ref / knn / size_hint /
  build / close / recreate_after_import.

What deliberately STAYS OUTSIDE (type-independent machinery):
  aux naming + predicates, the hidden vec_idx_id column, the label
  counter and its persistence, prebuilt fetch-time capture, the
  trx-rollback plumbing (implementations record into it or not), the
  memory budget. Aux-table DDL lifecycle (create/drop/rename/detach)
  also stays direct until commit S1 introduces the second aux schema.

The R1 couplings are retired by R2: dict_table_t::vec is the generic
Vec_runtime base (identity fields only — the gates and field_no/dims
peeks at the call sites are type-agnostic reads of it), and each
runtime carries a back-pointer to the Vector_index implementation that
allocated it, so dispatch never re-resolves the TYPE while a runtime
is open. Only the allocating implementation may interpret the
subtype (vec_t for hnsw). */

#ifndef vec0index_h
#define vec0index_h

#include "vec0aux.h"

/** The registered vector index TYPEs. The enum value is the identity
used everywhere the type is already known (registry indexing, runtime
dispatch, future switch()es); the string token exists only at the
boundaries where SQL hands us text — DDL validation and the
KEY::vector_index_type reloaded from the DD (SPANN R3). */
enum class Vec_index_type : uint8_t {
  HNSW = 0,
  /* SPANN = 1 — added by commit S1 */
};

/** Per-TYPE vector-index runtime operations. Implementations are
STATELESS singletons — all per-index state lives in the dict_table_t
companion (vec_t for hnsw), so there is no lifetime to manage here. */
class Vector_index {
 public:
  virtual ~Vector_index() = default;

  /** @return this implementation's registered TYPE */
  [[nodiscard]] virtual Vec_index_type type() const = 0;

  /** Get-or-create the per-table companion (lazy). See vec_open. */
  virtual void open(dict_table_t *table, uint16_t field_no, uint32_t dims,
                    int M, int ef_construction) const = 0;

  /** Build/refresh the in-memory runtime from the aux. See vec_load. */
  [[nodiscard]] virtual dberr_t load(dict_table_t *table, THD *thd) const = 0;

  /** Index one new point on the USER transaction. See vec_insert_point. */
  [[nodiscard]] virtual dberr_t insert(trx_t *trx, dict_table_t *table,
                                       THD *thd, uint64_t label,
                                       const float *vec_data,
                                       const byte *row_ref,
                                       ulint row_ref_len) const = 0;

  /** Retire one point (DELETE, or the delete half of an UPDATE) on the
  USER transaction. See vec_delete_point. */
  [[nodiscard]] virtual dberr_t remove(trx_t *trx, dict_table_t *table,
                                       THD *thd, uint64_t label) const = 0;

  /** PK changed, vector unchanged: repoint the stored base-row
  reference. See vec_refresh_row_ref. */
  [[nodiscard]] virtual dberr_t refresh_row_ref(trx_t *trx, dict_table_t *table,
                                                THD *thd, uint64_t label,
                                                const byte *row_ref,
                                                ulint row_ref_len) const = 0;

  /** Approximate kNN: candidates, closer-first; caller applies its own
  read-view visibility per hit. `exclude` is the resumable-search hook.
  See vec_knn_search. */
  [[nodiscard]] virtual dberr_t knn(
      dict_table_t *table, THD *thd, const float *query, uint32_t dims,
      size_t k, size_t ef, std::vector<vec_knn_hit_t> *hits,
      const std::unordered_set<uint64_t> *exclude = nullptr) const = 0;

  /** Upper bound for the resume loop: when a search has already
  spanned this many candidates, widening cannot find more. */
  [[nodiscard]] virtual size_t size_hint(const dict_table_t *table) const = 0;

  /** Free the in-memory runtime. Safe on tables that never opened
  one. See vec_close. */
  virtual void close(dict_table_t *table) const = 0;

  /** Re-mint empty aux state after IMPORT TABLESPACE. See
  vec_aux_recreate_after_import. */
  [[nodiscard]] virtual dberr_t recreate_after_import(dict_table_t *table,
                                                      trx_t *trx) const = 0;
};

/** Look up the implementation registered under a TYPE token, e.g. the
"hnsw" of CREATE ... VECTOR KEY (v) TYPE hnsw (case-insensitive; the
token is not necessarily NUL-terminated). String lookup is for the
boundaries only — DDL validation (parse_options) and the sites reading
KEY::vector_index_type at open/build; once the type is known, use
vec_index_by_enum(). Registering a new TYPE is one entry in
vec_type_registry[] (vec0index.cc); rejecting unknown TYPEs at
CREATE/ALTER is what keeps them out of every engine path.
@return the implementation, or nullptr for an unknown TYPE */
[[nodiscard]] const Vector_index *vec_index_by_name(const char *token,
                                                    size_t len);

/** The implementation for a known TYPE — O(1), never nullptr. */
[[nodiscard]] const Vector_index *vec_index_by_enum(Vec_index_type type);

/** The registered token for a known TYPE, e.g. "hnsw" — the string
embedded in aux table names (vec_<token>_<tid>_<iid>, SPANN R4) and
printed by SHOW CREATE. Tokens are lowercase ASCII identifiers and
MUST NOT contain '_' (the aux-name field separator). */
[[nodiscard]] const char *vec_index_token(Vec_index_type type);

/** Resolve the runtime for `table`'s vector index.

R1: always the HNSW singleton — the only registered TYPE — and
therefore safe to call for ANY table (close() etc. no-op on tables
without a vector runtime, preserving today's semantics). R2 turns this
into a registry lookup keyed by the index's TYPE token.
@return never nullptr */
[[nodiscard]] const Vector_index *vec_index_for(const dict_table_t *table);

#endif /* vec0index_h */
