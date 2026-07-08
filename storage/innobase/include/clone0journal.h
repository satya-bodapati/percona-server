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
 Backup DDL journal: append-only journal of DDL notifications for external
 physical backup tools (Percona XtraBackup delta backup mode).

 While enabled (innodb_backup_ddl_journal = ON), every Clone_notify
 construction/destruction appends one line to
 <datadir>/#ib_backup_tracking/ddl_journal. The begin/end pair brackets the
 LSN of the underlying operation's redo record; consumers must rely on
 ordering and presence only, never on exact record LSNs. */

#ifndef clone0journal_h
#define clone0journal_h

#include <atomic>
#include <cstdint>
#include <mutex>

#include "clone0api.h"
#include "univ.i"

/** Directory (relative to datadir) holding backup tracking artifacts. */
constexpr char BACKUP_TRACKING_DIR[] = "#ib_backup_tracking";

/** DDL journal file name inside BACKUP_TRACKING_DIR. */
constexpr char BACKUP_DDL_JOURNAL_FILE[] = "ddl_journal";

/** Append-only DDL notification journal for backup tools.

 Line format (tab separated, one event per line):
   <seq> <BEGIN|END> <type> <space_id> <lsn> <path>
 where <lsn> is log_get_lsn() at append time (a bracket bound, not the
 record LSN) and <path> is the tablespace's first file path resolved at
 append time (empty when the space does not exist at that moment, e.g. the
 BEGIN event of a CREATE or any event after a DROP completed).

 A header line "# ddl_journal v1 enabled_at <lsn>" is written on enable. */
class Backup_ddl_journal {
 public:
  /** Enable the journal: create the tracking directory and truncate/create
  the journal file, then start accepting events.
  @return true on success */
  bool enable();

  /** Disable the journal: stop accepting events, close and delete the
  journal file. Idempotent. */
  void disable();

  /** @return true iff the journal is accepting events. */
  bool is_active() const { return m_active.load(std::memory_order_acquire); }

  /** Append one notification event. No-op when inactive or for the system
  temporary tablespace.
  @param[in] type   notification type
  @param[in] space  tablespace id the notification is for
  @param[in] begin  true for Clone_notify construction, false destruction */
  void log(Clone_notify::Type type, space_id_t space, bool begin);

 private:
  /** Serializes appends and enable/disable transitions. */
  std::mutex m_mutex;

  /** Event sequence number, reset on enable. */
  uint64_t m_seq{0};

  /** OS file descriptor of the journal, -1 when closed. */
  int m_fd{-1};

  /** Fast-path gate checked before taking m_mutex. */
  std::atomic<bool> m_active{false};
};

/** The global backup DDL journal instance. */
extern Backup_ddl_journal backup_ddl_journal;

/** Sysvar backing innodb_backup_ddl_journal. */
extern bool srv_backup_ddl_journal;

#endif /* clone0journal_h */
