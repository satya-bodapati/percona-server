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

 Model: each registered session owns its own append-only slice file,
 <datadir>/#ib_backup_tracking/ddl_journal.<backup_id>. Every DDL
 notification is appended (fanned out) to every open session's file, so a
 session's file holds exactly the events that occurred while it was
 registered. Cutting a session flushes its file to disk and returns the path
 for the backup tool to read; stopping removes it. Sessions are fully
 independent — there is no shared stream and no shared byte offset, so a
 write fault on one session's file can never corrupt another's.

 SQL surface (UDFs registered by InnoDB, like innodb_redo_log_consumer_*):
   innodb_backup_ddl_journal_start()      -> backup_id (0 on error)
   innodb_backup_ddl_journal_cut(id)      -> slice file path ('' on error)
   innodb_backup_ddl_journal_stop(id)     -> 0 on success

 File format: JSONL — one JSON object per line.
   {"event":"header","version":2,"backup_id":N,"start_lsn":N}
   {"seq":N,"ev":"BEGIN"|"END","type":"SPACE_RENAME","space":N,"flags":N,
    "lsn":N,"path":"./test/t1.ibd"}
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

/** Base name of per-session slice files inside BACKUP_TRACKING_DIR. A
session's slice is "ddl_journal.<backup_id>". */
constexpr char BACKUP_DDL_JOURNAL_FILE[] = "ddl_journal";

/** Session-based DDL notification journal for backup tools. */
class Backup_ddl_journal {
 public:
  /** Register a session: create its slice file, write the header line, and
  enable event capture.
  @param[out] backup_id  minted session id
  @return true on success */
  bool session_start(uint64_t *backup_id);

  /** Flush the session's slice file to disk and return its path. The file
  already holds every event appended since the session started; may be called
  more than once.
  @param[in]  backup_id   session id from session_start()
  @param[out] slice_path  path of the slice file, relative to datadir
  @return true on success (unknown session id, or a session whose slice hit a
  write fault, is an error) */
  bool session_cut(uint64_t backup_id, std::string *slice_path);

  /** Unregister a session and delete its slice file. The last session
  disables event capture.
  @param[in] backup_id  session id from session_start()
  @return true on success (unknown session id is an error) */
  bool session_stop(uint64_t backup_id);

  /** Append one notification event to every open session's slice. No-op when
  no session is registered or for the system temporary tablespace.
  @param[in] type   notification type
  @param[in] space  tablespace id the notification is for
  @param[in] begin  true for Clone_notify construction, false destruction */
  void log(Clone_notify::Type type, space_id_t space, bool begin);

  /** Append a self-contained point event: a BEGIN immediately followed by an
  END for the same type/space. Use where there is no Clone_notify RAII scope
  to bracket the operation (e.g. the IMPORT hook synthesizing a SPACE_CREATE
  for the imported space).
  @param[in] type   notification type
  @param[in] space  tablespace id the notification is for */
  void log_point(Clone_notify::Type type, space_id_t space);

  /** Shutdown: drop all sessions and files. */
  void deinit();

 private:
  /** State of one registered backup session. */
  struct Session {
    /** OS file descriptor of the session's slice file, -1 when closed. */
    int fd{-1};
    /** Event sequence number within this session's slice. */
    uint64_t seq{0};
    /** Bytes confirmed written to the slice (== its on-disk size). A record
    is written all-or-nothing: a short write is rolled back to this size. */
    uint64_t bytes{0};
    /** Set if a write to the slice was short/failed. The slice is then
    incomplete and cut() must fail so the backup aborts rather than trust it. */
    bool broken{false};
  };

  /** @return path of a session's slice file, relative to datadir */
  static std::string slice_path(uint64_t backup_id);

  /** Append a complete line to a session's slice. On a short/failed write,
  roll the partial bytes back (keeping the file whole-line aligned) and mark
  the session broken. Caller holds m_mutex. */
  void append_record(Session &session, const std::string &line);

  /** Serializes appends and session transitions. */
  std::mutex m_mutex;

  /** Session id generator (seeded once from the wall clock so ids do not
  collide with orphan slice files from a previous server run). */
  uint64_t m_next_id{0};

  /** Registered sessions: backup_id -> session state. */
  std::map<uint64_t, Session> m_sessions;

  /** Fast-path gate checked by log() before taking m_mutex; true while at
  least one session is registered. */
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
