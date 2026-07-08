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

/** @file clone/clone0journal.cc
 Backup DDL journal used by Percona XtraBackup delta backup mode. See
 include/clone0journal.h for the format and semantics. */

#include "clone0journal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "fil0fil.h"
#include "fsp0fsp.h"
#include "log0log.h"
#include "os0file.h"
#include "ut0new.h"

Backup_ddl_journal backup_ddl_journal;

bool srv_backup_ddl_journal = false;

/** @return journal token for a notification type. */
static const char *type_str(Clone_notify::Type type) {
  switch (type) {
    case Clone_notify::Type::SPACE_CREATE:
      return "SPACE_CREATE";
    case Clone_notify::Type::SPACE_DROP:
      return "SPACE_DROP";
    case Clone_notify::Type::SPACE_RENAME:
      return "SPACE_RENAME";
    case Clone_notify::Type::SPACE_IMPORT:
      return "SPACE_IMPORT";
    case Clone_notify::Type::SPACE_ALTER_ENCRYPT:
      return "SPACE_ALTER_ENCRYPT";
    case Clone_notify::Type::SPACE_ALTER_ENCRYPT_GENERAL:
      return "SPACE_ALTER_ENCRYPT_GENERAL";
    case Clone_notify::Type::SPACE_ALTER_ENCRYPT_GENERAL_FLAGS:
      return "SPACE_ALTER_ENCRYPT_GENERAL_FLAGS";
    case Clone_notify::Type::SPACE_ALTER_INPLACE:
      return "SPACE_ALTER_INPLACE";
    case Clone_notify::Type::SPACE_ALTER_INPLACE_BULK:
      return "SPACE_ALTER_INPLACE_BULK";
    case Clone_notify::Type::SPACE_UNDO_DDL:
      return "SPACE_UNDO_DDL";
    case Clone_notify::Type::SYSTEM_REDO_DISABLE:
      return "SYSTEM_REDO_DISABLE";
  }
  return "UNKNOWN";
}

/** Write a fully formatted line to the journal fd. Caller holds m_mutex. */
static void write_line(int fd, const char *buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t n = ::write(fd, buf + done, len - done);
    if (n <= 0) {
      /* Journal write failure must not break the DDL itself; the backup
      tool detects a short journal via the missing END events. */
      break;
    }
    done += static_cast<size_t>(n);
  }
}

bool Backup_ddl_journal::enable() {
  std::lock_guard<std::mutex> guard(m_mutex);

  if (m_fd >= 0) {
    return true;
  }

  /* Server CWD is the datadir; keep paths relative so the file lands where
  the backup tool expects it. */
  os_file_create_directory(BACKUP_TRACKING_DIR, false);

  const std::string path =
      std::string(BACKUP_TRACKING_DIR) + '/' + BACKUP_DDL_JOURNAL_FILE;

  m_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);

  if (m_fd < 0) {
    ib::error() << "Backup DDL journal: cannot create " << path;
    return false;
  }

  m_seq = 0;

  char buf[128];
  const int len =
      snprintf(buf, sizeof(buf), "# ddl_journal v1 enabled_at " LSN_PF "\n",
               log_get_lsn(*log_sys));
  write_line(m_fd, buf, static_cast<size_t>(len));

  m_active.store(true, std::memory_order_release);

  ib::info() << "Backup DDL journal enabled: " << path;

  return true;
}

void Backup_ddl_journal::disable() {
  std::lock_guard<std::mutex> guard(m_mutex);

  if (m_fd < 0) {
    return;
  }

  m_active.store(false, std::memory_order_release);

  ::close(m_fd);
  m_fd = -1;

  const std::string path =
      std::string(BACKUP_TRACKING_DIR) + '/' + BACKUP_DDL_JOURNAL_FILE;
  ::unlink(path.c_str());

  ib::info() << "Backup DDL journal disabled";
}

void Backup_ddl_journal::log(Clone_notify::Type type, space_id_t space,
                             bool begin) {
  if (!is_active()) {
    return;
  }

  if (fsp_is_system_temporary(space)) {
    return;
  }

  /* Resolve the tablespace file path at this moment; may legitimately be
  unknown (BEGIN of a CREATE, anything after a completed DROP, or the
  pseudo space id of SYSTEM_REDO_DISABLE). */
  char *path = fil_space_get_first_path(space);

  const lsn_t lsn = log_get_lsn(*log_sys);

  char buf[FN_REFLEN + 128];

  std::lock_guard<std::mutex> guard(m_mutex);

  if (m_fd < 0) {
    return;
  }

  ++m_seq;

  const int len = snprintf(
      buf, sizeof(buf), UINT64PF "\t%s\t%s\t" SPACE_ID_PF "\t" LSN_PF "\t%s\n",
      m_seq, begin ? "BEGIN" : "END", type_str(type), space, lsn,
      path != nullptr ? path : "");

  write_line(m_fd, buf, static_cast<size_t>(len));

  if (path != nullptr) {
    ut::free(path);
  }
}
