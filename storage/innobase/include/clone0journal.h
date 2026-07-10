/*****************************************************************************

Copyright (c) 2026, Percona and/or its affiliates.

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

/** @file include/clone0journal.h
 Backup DDL journal: session-based journal of DDL notifications for external
 physical backup tools (Percona XtraBackup delta backup mode).

 Model (mirrors the page tracking component): the server appends every DDL
 notification once to a single internal journal stream while at least one
 session is registered. Each session remembers the stream offset at its
 start. Cutting a session materializes its slice — everything appended since
 the session started — into <datadir>/#ib_backup_tracking/ddl_journal.<id>
 for the backup tool to read. Concurrent sessions therefore never interfere;
 they read overlapping slices of the same stream.

 SQL surface (UDFs registered by InnoDB, like innodb_redo_log_consumer_*):
   innodb_backup_ddl_journal_start()      -> backup_id (0 on error)
   innodb_backup_ddl_journal_cut(id)      -> slice file path ('' on error)
   innodb_backup_ddl_journal_stop(id)     -> 0 on success

 File format: JSONL — one JSON object per line.
   {"event":"header","version":2,"backup_id":N,"start_lsn":N}
   {"seq":N,"ev":"BEGIN"|"END","type":"SPACE_RENAME","space":N,"lsn":N,
    "path":"./test/t1.ibd"}
 The BEGIN/END pair brackets the LSN of the underlying operation's redo
 records; consumers must rely on ordering and presence only, never on exact
 record LSNs. Paths are JSON-escaped and resolved at append time (empty when
 the space does not exist at that moment). */

#ifndef clone0journal_h
#define clone0journal_h

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "clone0api.h"
#include "univ.i"

/** Directory (relative to datadir) holding backup tracking artifacts. */
constexpr char BACKUP_TRACKING_DIR[] = "#ib_backup_tracking";

/** Base name of journal files inside BACKUP_TRACKING_DIR. The internal
stream is "ddl_journal"; per-session slices are "ddl_journal.<backup_id>". */
constexpr char BACKUP_DDL_JOURNAL_FILE[] = "ddl_journal";

/** Session-based DDL notification journal for backup tools. */
class Backup_ddl_journal {
 public:
  /** Register a session. The first session creates/truncates the internal
  journal stream and enables event capture.
  @param[out] backup_id  minted session id
  @return true on success */
  bool session_start(uint64_t *backup_id);

  /** Materialize the session's slice (all events appended since the session
  started) into ddl_journal.<backup_id>. May be called more than once; each
  call rewrites the slice with the events up to now.
  @param[in]  backup_id   session id from session_start()
  @param[out] slice_path  path of the slice file, relative to datadir
  @return true on success (unknown session id is an error) */
  bool session_cut(uint64_t backup_id, std::string *slice_path);

  /** Unregister a session and delete its slice file. The last session
  disables event capture and deletes the internal journal stream.
  @param[in] backup_id  session id from session_start()
  @return true on success (unknown session id is an error) */
  bool session_stop(uint64_t backup_id);

  /** Append one notification event. No-op when no session is registered or
  for the system temporary tablespace.
  @param[in] type   notification type
  @param[in] space  tablespace id the notification is for
  @param[in] begin  true for Clone_notify construction, false destruction */
  void log(Clone_notify::Type type, space_id_t space, bool begin);

  /** Shutdown: drop all sessions and files. */
  void deinit();

 private:
  /** @return path of the internal journal stream, relative to datadir */
  static std::string internal_path();

  /** @return path of a session's slice file, relative to datadir */
  static std::string slice_path(uint64_t backup_id);

  /** Close and delete the internal stream. Caller holds m_mutex. */
  void close_internal();

  /** Serializes appends and session transitions. */
  std::mutex m_mutex;

  /** Event sequence number within the current internal stream. */
  uint64_t m_seq{0};

  /** Byte size of the internal stream (append position). */
  uint64_t m_offset{0};

  /** OS file descriptor of the internal stream, -1 when closed. */
  int m_fd{-1};

  /** Session id generator (seeded once from the wall clock so ids do not
  collide with orphan slice files from a previous server run). */
  uint64_t m_next_id{0};

  /** Registered sessions: backup_id -> internal stream offset at start. */
  std::map<uint64_t, uint64_t> m_sessions;

  /** Fast-path gate checked by log() before taking m_mutex. */
  std::atomic<bool> m_active{false};
};

/** The global backup DDL journal instance. */
extern Backup_ddl_journal backup_ddl_journal;

/** Register the innodb_backup_ddl_journal_* UDFs. Called when the log
subsystem starts (see meb::redo_log_archive_init for the pattern). */
void backup_ddl_journal_init();

/** Unregister the UDFs and drop journal state. */
void backup_ddl_journal_deinit();

#endif /* clone0journal_h */
