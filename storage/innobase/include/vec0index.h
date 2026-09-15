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

#pragma once

struct dict_index_t;

/** In-memory state belonging to one open vector index - the graph, the
arena its nodes live in, the persistor, and the parameters read back from
the DD.

Per INDEX, not per table. Everything it holds is a property of a single
index: the dimension, the distance metric, M, ef_construction, the entry
point, the label space, the graph itself. Two vector indexes on the same
table share none of it, and the aux table name is already keyed by index
id. FTS hangs its state off dict_table_t because FTS genuinely has
table-scoped state (one shared cache, one FTS_DOC_ID column, one delete
list) plus a list of the indexes sharing it; we have no equivalent.

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

#ifndef UNIV_HOTBACKUP
/** Release the mutex and event that back vec_runtime_get_or_wait()
(vec0hnsw.h) for one index, if vec_runtime_open()/
vec_runtime_get_or_wait() (vec0hnsw.cc) ever lazily created them. Called
from dict_mem_index_free(), next to vec_index_runtime_free() above and
dict_index_zip_pad_mutex_destroy() (dict0mem.h) - dict_index_t has no
destructor, so whatever those two allocated has to be released by hand or
it leaks on every DROP INDEX/DROP TABLE, for the rest of the server's
life, for a vector index.
@param[in,out]  index  index whose vec_open_mutex/vec_open_event to free */
void vec_open_sync_free(dict_index_t *index);
#endif /* !UNIV_HOTBACKUP */
