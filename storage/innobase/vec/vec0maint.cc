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

/** @file vec/vec0maint.cc
The vector-index maintenance thread. See vec0maint.h for the model. */

#include "vec0maint.h"

#include <chrono>
#include <mutex>
#include <set>
#include <thread>

#include "current_thd.h"
#include "dict0dd.h"
#include "dict0dict.h"
#include "os0thread-create.h"
#include "sql/sql_thd_internal_api.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "ut0wqueue.h"
#include "vec0spann.h"

namespace {

/** One queued job. Allocated on its own heap (the fts_msg_t idiom);
the queue owns and frees the heap after processing. */
struct vec_maint_msg_t {
  Vec_maint_op op;
  table_id_t table_id;
  uint64_t head_id;
  /** synchronous-caller hooks (tests); may be nullptr */
  os_event_t done;
  dberr_t *result;
  mem_heap_t *heap;
};

ib_wqueue_t *vec_maint_wq = nullptr;

/** Guards the ban set and the running marker. */
std::mutex vec_maint_mutex;
/** Banned table ids: queued jobs are skipped, running jobs abort.
Entries are lifted by vec_maint_allow (aux re-created) or reaped when
the table is gone for good. */
std::set<table_id_t> vec_maint_banned;
/** table_id of the job being executed, or 0. */
table_id_t vec_maint_running = 0;

/** Queue-wait quantum; also the shutdown-poll period. */
constexpr std::chrono::seconds VEC_MAINT_WAIT{1};

vec_maint_msg_t *vec_maint_create_msg(Vec_maint_op op, table_id_t table_id,
                                      uint64_t head_id, os_event_t done,
                                      dberr_t *result) {
  mem_heap_t *heap = mem_heap_create(128, UT_LOCATION_HERE);
  auto *msg = static_cast<vec_maint_msg_t *>(
      mem_heap_alloc(heap, sizeof(vec_maint_msg_t)));
  msg->op = op;
  msg->table_id = table_id;
  msg->head_id = head_id;
  msg->done = done;
  msg->result = result;
  msg->heap = heap;
  return msg;
}

/** Execute one job against its (still existing, not banned) table.
Deliberately NO MDL (the mdl=nullptr form): a shared MDL held across a
long job would stall any concurrent DDL for the job's whole duration.
DDL safety is the ban protocol instead — every aux-dropping path bans
the table and waits, the job polls the ban and aborts within a batch —
with the dict reference from this open keeping the table object
itself alive (the fts_optimize model). */
dberr_t vec_maint_run_job(const vec_maint_msg_t *msg, THD *thd) {
  dict_table_t *table =
      dd_table_open_on_id(msg->table_id, thd, nullptr, false, false);
  if (table == nullptr) {
    return DB_TABLE_NOT_FOUND;
  }

  dberr_t err = DB_UNSUPPORTED;
  switch (msg->op) {
    case Vec_maint_op::RESAMPLE:
      err = vec_spann_resample(table, thd);
      break;
    case Vec_maint_op::SPLIT:
      err = vec_spann_split(table, thd, msg->head_id);
      break;
    case Vec_maint_op::MERGE:
      err = vec_spann_merge(table, thd);
      break;
    case Vec_maint_op::GC:
      err = vec_spann_gc(table, thd);
      break;
    case Vec_maint_op::STOP:
      ut_d(ut_error);
      break;
  }

  dd_table_close(table, thd, nullptr, false);
  return err;
}

void vec_maint_thread() {
  /* The jobs need a THD: the aux-open DD fallback takes MDL, and the
  DEBUG_SYNC choreography of the L-phase tests runs through it. */
  THD *thd = create_internal_thd();

  for (;;) {
    auto *msg = static_cast<vec_maint_msg_t *>(
        ib_wqueue_timedwait(vec_maint_wq, VEC_MAINT_WAIT));

    if (msg == nullptr) {
      if (srv_shutdown_state.load() >=
          SRV_SHUTDOWN_MASTER_STOP) { /* defensive: STOP is the normal exit */
        break;
      }
      continue;
    }

    if (msg->op == Vec_maint_op::STOP) {
      if (msg->done != nullptr) {
        os_event_set(msg->done);
      }
      mem_heap_free(msg->heap);
      break;
    }

    dberr_t err;
    bool banned;
    {
      std::lock_guard<std::mutex> g(vec_maint_mutex);
      banned = vec_maint_banned.count(msg->table_id) != 0;
      if (!banned) {
        vec_maint_running = msg->table_id;
      }
    }

    if (banned) {
      err = DB_INTERRUPTED;
    } else {
      err = vec_maint_run_job(msg, thd);
      {
        std::lock_guard<std::mutex> g(vec_maint_mutex);
        vec_maint_running = 0;
      }
    }

    if (msg->result != nullptr) {
      *msg->result = err;
    }
    if (msg->done != nullptr) {
      os_event_set(msg->done);
    }
    mem_heap_free(msg->heap);
  }

  destroy_internal_thd(thd);
}

}  // namespace

void vec_maint_init() {
  ut_ad(!srv_read_only_mode);
  ut_a(vec_maint_wq == nullptr);

  vec_maint_wq = ib_wqueue_create();
  ut_a(vec_maint_wq != nullptr);

  srv_threads.m_vec_maint =
      os_thread_create(vec_maint_thread_key, 0, vec_maint_thread);
  srv_threads.m_vec_maint.start();
}

void vec_maint_shutdown() {
  if (vec_maint_wq == nullptr) {
    return;
  }

  auto *msg = vec_maint_create_msg(Vec_maint_op::STOP, 0, 0, nullptr, nullptr);
  ib_wqueue_add(vec_maint_wq, msg, msg->heap);

  srv_threads.m_vec_maint.join();

  /* Drain anything enqueued after STOP: report and release waiters. */
  while (!ib_wqueue_is_empty(vec_maint_wq)) {
    auto *m = static_cast<vec_maint_msg_t *>(
        ib_wqueue_timedwait(vec_maint_wq, std::chrono::seconds{0}));
    if (m == nullptr) {
      break;
    }
    if (m->result != nullptr) {
      *m->result = DB_INTERRUPTED;
    }
    if (m->done != nullptr) {
      os_event_set(m->done);
    }
    mem_heap_free(m->heap);
  }

  ib_wqueue_free(vec_maint_wq);
  vec_maint_wq = nullptr;
}

void vec_maint_enqueue(Vec_maint_op op, table_id_t table_id, uint64_t head_id,
                       os_event_t done, dberr_t *result) {
  if (vec_maint_wq == nullptr) {
    if (result != nullptr) {
      *result = DB_INTERRUPTED;
    }
    if (done != nullptr) {
      os_event_set(done);
    }
    return;
  }
  auto *msg = vec_maint_create_msg(op, table_id, head_id, done, result);
  ib_wqueue_add(vec_maint_wq, msg, msg->heap);
}

void vec_maint_cancel_and_wait(table_id_t table_id) {
  if (vec_maint_wq == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> g(vec_maint_mutex);
    vec_maint_banned.insert(table_id);
  }
  /* Running jobs poll vec_maint_is_canceled between batches and roll
  back; the waits here are DDL-path only and short. */
  for (;;) {
    {
      std::lock_guard<std::mutex> g(vec_maint_mutex);
      if (vec_maint_running != table_id) {
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

void vec_maint_allow(table_id_t table_id) {
  std::lock_guard<std::mutex> g(vec_maint_mutex);
  vec_maint_banned.erase(table_id);
}

bool vec_maint_is_canceled(table_id_t table_id) {
  std::lock_guard<std::mutex> g(vec_maint_mutex);
  return vec_maint_banned.count(table_id) != 0;
}
