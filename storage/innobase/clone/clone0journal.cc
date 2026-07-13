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
 include/clone0journal.h for the model and format. */

#include "clone0journal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "fil0fil.h"
#include "fsp0fsp.h"
#include "log0log.h"
#include "mysql/components/services/udf_registration.h"
#include "os0file.h"
#include "srv0dynamic_procedures.h"
#include "ut0new.h"

Backup_ddl_journal backup_ddl_journal;

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

/** JSON-escape a string (backslash, double quote and control characters). */
static std::string json_escape(const char *s) {
  std::string out;
  if (s == nullptr) {
    return out;
  }
  for (const char *p = s; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

/** Write a fully formatted buffer to fd; short/failed writes are dropped
(the backup tool detects an incomplete journal via unpaired events). */
static void write_all(int fd, const char *buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t n = ::write(fd, buf + done, len - done);
    if (n <= 0) {
      break;
    }
    done += static_cast<size_t>(n);
  }
}

std::string Backup_ddl_journal::internal_path() {
  return std::string(BACKUP_TRACKING_DIR) + '/' + BACKUP_DDL_JOURNAL_FILE;
}

std::string Backup_ddl_journal::slice_path(uint64_t backup_id) {
  return internal_path() + '.' + std::to_string(backup_id);
}

void Backup_ddl_journal::close_internal() {
  if (m_fd >= 0) {
    m_active.store(false, std::memory_order_release);
    ::close(m_fd);
    m_fd = -1;
    ::unlink(internal_path().c_str());
  }
}

bool Backup_ddl_journal::session_start(uint64_t *backup_id) {
  std::lock_guard<std::mutex> guard(m_mutex);

  if (m_sessions.empty()) {
    /* First session: create/truncate the internal stream. Server CWD is the
    datadir; keep paths relative so files land where the backup tool
    expects them. */
    os_file_create_directory(BACKUP_TRACKING_DIR, false);

    /* O_RDWR: session_cut() reads the stream back via pread(). */
    m_fd = ::open(internal_path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0640);
    if (m_fd < 0) {
      ib::error() << "Backup DDL journal: cannot create " << internal_path();
      return false;
    }

    m_seq = 0;
    m_offset = 0;
    m_active.store(true, std::memory_order_release);
  }

  if (m_next_id == 0) {
    /* Seed once per server run so ids do not collide with orphan slices
    from a previous run. */
    m_next_id = static_cast<uint64_t>(::time(nullptr)) * 1000;
  }
  const uint64_t id = ++m_next_id;

  m_sessions[id] = m_offset;

  ib::info() << "Backup DDL journal: session " << id << " started at offset "
             << m_offset << " (lsn " << log_get_lsn(*log_sys) << ")";

  *backup_id = id;
  return true;
}

bool Backup_ddl_journal::session_cut(uint64_t backup_id,
                                     std::string *slice_out) {
  std::lock_guard<std::mutex> guard(m_mutex);

  const auto it = m_sessions.find(backup_id);
  if (it == m_sessions.end() || m_fd < 0) {
    ib::error() << "Backup DDL journal: cut for unknown session " << backup_id;
    return false;
  }

  const uint64_t start_offset = it->second;

  /* Read the session's byte range from the internal stream. Appends are
  line-atomic under m_mutex, so [start_offset, m_offset) is a whole number
  of JSONL lines. */
  std::vector<char> buf(m_offset - start_offset);
  size_t got = 0;
  while (got < buf.size()) {
    const ssize_t n = ::pread(m_fd, buf.data() + got, buf.size() - got,
                              static_cast<off_t>(start_offset + got));
    if (n <= 0) {
      ib::error() << "Backup DDL journal: cannot read the journal stream";
      return false;
    }
    got += static_cast<size_t>(n);
  }

  const std::string slice = slice_path(backup_id);

  const int sfd = ::open(slice.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
  if (sfd < 0) {
    ib::error() << "Backup DDL journal: cannot create slice " << slice;
    return false;
  }

  char header[160];
  const int hlen = snprintf(header, sizeof(header),
                            "{\"event\":\"header\",\"version\":2,"
                            "\"backup_id\":" UINT64PF ",\"lsn\":" LSN_PF "}\n",
                            backup_id, log_get_lsn(*log_sys));
  write_all(sfd, header, static_cast<size_t>(hlen));
  write_all(sfd, buf.data(), buf.size());

  if (::fsync(sfd) != 0) {
    ib::warn() << "Backup DDL journal: fsync failed on slice " << slice;
  }
  ::close(sfd);

  ib::info() << "Backup DDL journal: session " << backup_id << " cut at offset "
             << m_offset << " into " << slice;

  *slice_out = slice;
  return true;
}

bool Backup_ddl_journal::session_stop(uint64_t backup_id) {
  std::lock_guard<std::mutex> guard(m_mutex);

  const auto it = m_sessions.find(backup_id);
  if (it == m_sessions.end()) {
    ib::error() << "Backup DDL journal: stop for unknown session " << backup_id;
    return false;
  }

  m_sessions.erase(it);
  ::unlink(slice_path(backup_id).c_str());

  if (m_sessions.empty()) {
    close_internal();
  }

  ib::info() << "Backup DDL journal: session " << backup_id << " stopped";
  return true;
}

void Backup_ddl_journal::log(Clone_notify::Type type, space_id_t space,
                             bool begin) {
  if (!m_active.load(std::memory_order_acquire)) {
    return;
  }

  if (fsp_is_system_temporary(space)) {
    return;
  }

  /* Resolve the tablespace file path at this moment; may legitimately be
  unknown (BEGIN of a CREATE, anything after a completed DROP, or the
  pseudo space id of SYSTEM_REDO_DISABLE). */
  char *path = fil_space_get_first_path(space);

  /* Record the tablespace flags authoritatively (0 when the space is not
  currently known, e.g. a BEGIN or a completed DROP). Backup tools need these
  for a reimported space (its final id may never have been file-copied) so
  they do not have to guess the flags. */
  const uint32_t space_flags = fil_space_get_flags(space);

  const lsn_t lsn = log_get_lsn(*log_sys);

  const std::string escaped = json_escape(path);

  if (path != nullptr) {
    ut::free(path);
  }

  char buf[FN_REFLEN * 2 + 160];

  std::lock_guard<std::mutex> guard(m_mutex);

  if (m_fd < 0) {
    return;
  }

  ++m_seq;

  const int len = snprintf(
      buf, sizeof(buf),
      "{\"seq\":" UINT64PF
      ",\"ev\":\"%s\",\"type\":\"%s\",\"space\":%u,\"flags\":%u,"
      "\"lsn\":" LSN_PF ",\"path\":\"%s\"}\n",
      m_seq, begin ? "BEGIN" : "END", type_str(type),
      static_cast<unsigned>(space),
      space_flags == UINT32_UNDEFINED ? 0u : space_flags, lsn, escaped.c_str());

  write_all(m_fd, buf, static_cast<size_t>(len));
  m_offset += static_cast<uint64_t>(len);
}

void Backup_ddl_journal::deinit() {
  std::lock_guard<std::mutex> guard(m_mutex);
  for (const auto &s : m_sessions) {
    ::unlink(slice_path(s.first).c_str());
  }
  m_sessions.clear();
  close_internal();
}

/* ---------------------------- UDF surface ------------------------------ */

static bool ddl_journal_start_init(UDF_INIT *, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "innodb_backup_ddl_journal_start() takes no arguments");
    return true;
  }
  return false;
}

static void ddl_journal_start_deinit(UDF_INIT *) {}

static long long ddl_journal_start(UDF_INIT *, UDF_ARGS *, unsigned char *,
                                   unsigned char *) {
  uint64_t id = 0;
  if (!backup_ddl_journal.session_start(&id)) {
    return 0; /* 0 = error; valid ids are always non-zero */
  }
  return static_cast<long long>(id);
}

static bool ddl_journal_cut_init(UDF_INIT *initid, UDF_ARGS *args,
                                 char *message) {
  if (args->arg_count != 1 || args->arg_type[0] != INT_RESULT) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "innodb_backup_ddl_journal_cut() requires one integer argument "
             "(backup_id)");
    return true;
  }
  initid->maybe_null = true;
  initid->max_length = FN_REFLEN;
  initid->ptr = static_cast<char *>(malloc(FN_REFLEN + 1));
  return initid->ptr == nullptr;
}

static void ddl_journal_cut_deinit(UDF_INIT *initid) {
  if (initid->ptr != nullptr) {
    free(initid->ptr);
    initid->ptr = nullptr;
  }
}

static char *ddl_journal_cut(UDF_INIT *initid, UDF_ARGS *args, char *,
                             unsigned long *length, unsigned char *is_null,
                             unsigned char *) {
  const uint64_t id =
      static_cast<uint64_t>(*reinterpret_cast<long long *>(args->args[0]));

  std::string slice;
  if (!backup_ddl_journal.session_cut(id, &slice) ||
      slice.length() > FN_REFLEN) {
    *is_null = 1; /* NULL = error (e.g. unknown session id) */
    return nullptr;
  }

  memcpy(initid->ptr, slice.c_str(), slice.length());
  *length = slice.length();
  return initid->ptr;
}

static bool ddl_journal_stop_init(UDF_INIT *, UDF_ARGS *args, char *message) {
  if (args->arg_count != 1 || args->arg_type[0] != INT_RESULT) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "innodb_backup_ddl_journal_stop() requires one integer argument "
             "(backup_id)");
    return true;
  }
  return false;
}

static void ddl_journal_stop_deinit(UDF_INIT *) {}

static long long ddl_journal_stop(UDF_INIT *, UDF_ARGS *args, unsigned char *,
                                  unsigned char *) {
  const uint64_t id =
      static_cast<uint64_t>(*reinterpret_cast<long long *>(args->args[0]));

  /* 0 = success, 1 = error (e.g. unknown session id) */
  return backup_ddl_journal.session_stop(id) ? 0 : 1;
}

namespace {

class Journal_procedures : public srv::Dynamic_procedures {
 protected:
  std::vector<srv::dynamic_procedure_data_t> get_procedures() const override {
    return {{"innodb_backup_ddl_journal_start", ddl_journal_start,
             ddl_journal_start_init, ddl_journal_start_deinit},
            {"innodb_backup_ddl_journal_stop", ddl_journal_stop,
             ddl_journal_stop_init, ddl_journal_stop_deinit},
            {"innodb_backup_ddl_journal_cut", ddl_journal_cut,
             ddl_journal_cut_init, ddl_journal_cut_deinit}};
  }
  std::string get_module_name() const override {
    return "innodb_backup_ddl_journal";
  }
};

Journal_procedures s_journal_procedures;

}  // namespace

void backup_ddl_journal_init() {
  if (!s_journal_procedures.register_procedures()) {
    ib::warn() << "Backup DDL journal: could not register the"
               << " innodb_backup_ddl_journal_* UDFs";
  }
}

void backup_ddl_journal_deinit() {
  backup_ddl_journal.deinit();
  s_journal_procedures.unregister();
}
