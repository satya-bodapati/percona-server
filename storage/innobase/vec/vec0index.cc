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
@file vec/vec0index.cc
Per-index vector runtime.
*/

#include <atomic>

#include "vec0index.h"

#include "dict0mem.h"
#include "ut0new.h"

void vec_index_runtime_free(dict_index_t *index) {
  ut_ad(index != nullptr);
  if (index->vec == nullptr) return;

  /* Deleting through the base pointer; the virtual destructor is what
  makes that correct for a subtype allocated by an implementation. */
  Vec_runtime *runtime = index->vec;
  /* Release store for symmetry with vec_runtime_get()'s acquire load. No
  reader can be racing here: the index is out of the dictionary cache
  with ref_count 0 before dict_mem_index_free() is reached. */
  std::atomic_ref<Vec_runtime *>(index->vec)
      .store(nullptr, std::memory_order_release);
  ut::delete_(runtime);
}

#ifndef UNIV_HOTBACKUP
void vec_open_sync_free(dict_index_t *index) {
  ut_ad(index != nullptr);

  /* Same guard dict_index_zip_pad_mutex_destroy() (dict0mem.h) uses for
  zip_pad.mutex: only touch what os_once actually finished creating. No
  reader can be racing here for the same reason vec_index_runtime_free()
  above needs none - the index is out of the dictionary cache with
  ref_count 0 before dict_mem_index_free() is reached. */
  if (index->vec_open_sync_created != os_once::DONE) return;

  if (index->vec_open_mutex != nullptr) {
    mutex_free(index->vec_open_mutex);
    ut::delete_(index->vec_open_mutex);
    index->vec_open_mutex = nullptr;
  }
  if (index->vec_open_event != nullptr) {
    /* Takes the pointer by reference and nulls it. */
    os_event_destroy(index->vec_open_event);
  }
}
#endif /* !UNIV_HOTBACKUP */
