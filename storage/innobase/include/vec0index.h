/*****************************************************************************

Copyright (c) 2026, Percona Inc.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/**
@file include/vec0index.h
Per-index vector runtime: the in-memory state a vector index needs while it
is open, hanging off dict_index_t::vec.
*/

#ifndef vec0index_h
#define vec0index_h

#include "db0err.h"

struct dict_index_t;
struct TABLE_SHARE;

/** In-memory state belonging to one open vector index - the graph, the arena
its nodes live in, the persistor, and the parameters read back from the DD.

Per INDEX, not per table. Everything it holds is a property of a single
index: the dimension, the distance metric, M, ef_construction, the entry
point, the label space, the graph itself. Two vector indexes on the same
table share none of it, and the aux table name is already keyed by index id.

Typed as a base so that a second index TYPE is an addition rather than an
edit: only the implementation that allocated a runtime may interpret the
subtype, and that downcast stays file-local to the implementation. */
struct Vec_runtime {
  virtual ~Vec_runtime() = default;
};

/** Release the runtime attached to an index, if any, and clear the
pointer. Called from dict_mem_index_free().

dict_index_t is never constructed or destructed - the memory is zeroed by
mem_heap_zalloc and dict_mem_fill_index_struct() acts as the constructor -
so dict_index_t::vec is a raw pointer that starts null for free and has to
be released by hand here, the way destroy_fields_array() already is.
@param[in,out]  index  index whose runtime is to be freed */
void vec_index_runtime_free(dict_index_t *index);

/** Give a vector index its runtime, as the index is built: loaded from the
DD, created by CREATE TABLE or TRUNCATE, or added by an ALTER. Every path
that makes a vector dict_index_t has the definition the parameters live in
- the dimension on the VECTOR column, M and the metric on the KEY - so no
index is ever without a runtime, and nothing has to race to build one.

Cheap: only the parameters are stored. The graph is read from the aux on
the first statement that needs it (vec_runtime_load_once), not here, so a
table loaded for purge or statistics pays nothing for it.

When the definition cannot be read, the index is left without a runtime,
which is the whole of the state: every statement that needs the index
fails with ER_INDEX_CORRUPT, the table itself stays readable and
droppable, and the next load of the table tries again. The index is not
marked DICT_CORRUPT, which InnoDB would persist.
@param[in,out]  index  the vector index, already in the dictionary cache
@param[in]      share  the definition the index was built from
@return DB_SUCCESS, or DB_INDEX_CORRUPT */
dberr_t vec_runtime_create(dict_index_t *index, const TABLE_SHARE *share);

#endif /* vec0index_h */
