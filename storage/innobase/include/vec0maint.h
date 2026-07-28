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

/** @file include/vec0maint.h
The vector-index maintenance thread (SPANN plan, phase L): ONE
server-wide background thread with a work queue — the fts_optimize
analog — executing index-maintenance jobs (L0 re-sample, L1 split,
L2 merge/GC). Deliberately type-agnostic at the queue level: jobs
carry a table_id and an op; the dispatcher resolves the index TYPE.

Correctness never depends on this thread: every job is ordinary
transactional row DML on an internal transaction, and every race with
user DML degenerates to extra-work-right-answer (double-seen vectors
are absorbed by label dedup on the read path). One job runs at a time;
a worker pool behind the same queue is a later tuning decision.

DDL choreography (the fts_optimize_remove_table analog): paths that
drop or re-mint a table's aux set call vec_maint_cancel_and_wait()
first — queued jobs for the table are skipped and any running job is
interrupted and waited out — and the aux-create path calls
vec_maint_allow() to lift the ban. Between the two, no maintenance
job can touch the table's aux. */

#ifndef vec0maint_h
#define vec0maint_h

#include "db0err.h"
#include "dict0types.h"
#include "os0event.h"
#include "univ.i"

/** Maintenance job kinds. */
enum class Vec_maint_op : uint8_t {
  /** L0: background global re-sample — rebuild the head set from a
  snapshot of the current postings, catch up, swap. */
  RESAMPLE,
  /** L1: split ONE oversized list into two k-means halves. */
  SPLIT,
  /** L2: fold undersized lists into one merged head. */
  MERGE,
  /** L2: sweep garbage-list and drained-dead posting rows. */
  GC,
  /** thread shutdown sentinel */
  STOP,
};

/** Start the maintenance thread and its work queue (srv0start, after
the dictionary is up; not in read-only mode). */
void vec_maint_init();

/** Stop the thread and free the queue (srv shutdown). Safe when never
initialized. */
void vec_maint_shutdown();

/** Enqueue a job. `done`/`result` are optional: when given, the
worker signals `done` after the job and stores its outcome in
`result` (the synchronous-caller hook, used by tests). No-op (with
DB_INTERRUPTED reported) when the thread is not running.
@param[in]  op        job kind
@param[in]  table_id  base table
@param[in]  head_id   the list to operate on (SPLIT); 0 otherwise
@param[in]  done      event to signal on completion, or nullptr
@param[out] result    outcome sink, or nullptr */
void vec_maint_enqueue(Vec_maint_op op, table_id_t table_id, uint64_t head_id,
                       os_event_t done, dberr_t *result);

/** Ban maintenance on `table_id` and wait until no job for it is
running: queued jobs are skipped at dequeue, a running job is
interrupted (jobs poll vec_maint_is_canceled between batches) and
waited out. Called by every path that drops/re-mints the table's
vector aux set, BEFORE touching the aux. Safe when the thread is not
running. */
void vec_maint_cancel_and_wait(table_id_t table_id);

/** Lift the ban (aux set (re)created). */
void vec_maint_allow(table_id_t table_id);

/** Poll hook for long-running jobs: true when the job's table was
banned mid-flight and the job must abort (roll back) at the next
batch boundary. */
[[nodiscard]] bool vec_maint_is_canceled(table_id_t table_id);

#endif /* vec0maint_h */
