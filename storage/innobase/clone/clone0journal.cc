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

#include "my_rapidjson_size_t.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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

/** Write to fd until len bytes are done or the OS refuses; @return the number
of bytes actually written (< len only on a short/failed write, e.g. ENOSPC). */
static size_t write_all(int fd, const char *buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t n = ::write(fd, buf + done, len - done);
    if (n <= 0) {
      break;
    }
    done += static_cast<size_t>(n);
  }
  return done;
}

std::string Backup_ddl_journal::slice_path(uint64_t backup_id) {
  return std::string(BACKUP_TRACKING_DIR) + '/' + BACKUP_DDL_JOURNAL_FILE +
         '.' + std::to_string(backup_id);
}

void Backup_ddl_journal::append_record(Session &session,
                                       const std::string &line) {
  const size_t n = write_all(session.fd, line.data(), line.size());
  if (n == line.size()) {
    session.bytes += n;
    return;
  }
  /* Short/failed write: chop the partial tail so the file stays a whole
  number of JSONL lines, and mark the session so its cut() fails and the
  backup aborts rather than trust an incomplete journal. */
  if (::ftruncate(session.fd, static_cast<off_t>(session.bytes)) != 0) {
    /* best effort; the broken flag below is what actually protects us */
  }
  session.broken = true;
  ib::error() << "Backup DDL journal: short write to a session slice;"
              << " the slice is incomplete and the backup will be failed";
}

bool Backup_ddl_journal::session_start(uint64_t *backup_id) {
  std::lock_guard<std::mutex> guard(m_mutex);

  /* Server CWD is the datadir; keep paths relative so files land where the
  backup tool expects them. Directory creation is idempotent. */
  os_file_create_directory(BACKUP_TRACKING_DIR, false);

  if (m_next_id == 0) {
    /* Seed once per server run so ids do not collide with orphan slices
    from a previous run. */
    m_next_id = static_cast<uint64_t>(::time(nullptr)) * 1000;
  }
  const uint64_t id = ++m_next_id;

  const std::string path = slice_path(id);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
  if (fd < 0) {
    ib::error() << "Backup DDL journal: cannot create slice " << path;
    return false;
  }

  const lsn_t start_lsn = log_get_lsn(*log_sys);

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  w.StartObject();
  w.Key("event");
  w.String("header");
  w.Key("version");
  w.Uint(2);
  w.Key("backup_id");
  w.Uint64(id);
  w.Key("start_lsn");
  w.Uint64(start_lsn);
  w.EndObject();
  std::string header(sb.GetString(), sb.GetSize());
  header += '\n';

  if (write_all(fd, header.data(), header.size()) != header.size()) {
    ib::error() << "Backup DDL journal: cannot write header to " << path;
    ::close(fd);
    ::unlink(path.c_str());
    return false;
  }

  Session session;
  session.fd = fd;
  session.bytes = header.size();
  m_sessions[id] = session;

  m_active.store(true, std::memory_order_release);

  ib::info() << "Backup DDL journal: session " << id << " started (lsn "
             << start_lsn << ") into " << path;

  *backup_id = id;
  return true;
}

bool Backup_ddl_journal::session_cut(uint64_t backup_id,
                                     std::string *slice_out) {
  std::lock_guard<std::mutex> guard(m_mutex);

  const auto it = m_sessions.find(backup_id);
  if (it == m_sessions.end()) {
    ib::error() << "Backup DDL journal: cut for unknown session " << backup_id;
    return false;
  }

  Session &session = it->second;
  if (session.broken) {
    ib::error() << "Backup DDL journal: session " << backup_id
                << " slice is incomplete (write fault); failing cut";
    return false;
  }

  /* The slice file already holds every event appended since the session
  started; just make it durable and hand back its path. */
  if (::fsync(session.fd) != 0) {
    ib::warn() << "Backup DDL journal: fsync failed on session " << backup_id;
  }

  const std::string slice = slice_path(backup_id);
  ib::info() << "Backup DDL journal: session " << backup_id << " cut into "
             << slice;

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

  if (it->second.fd >= 0) {
    ::close(it->second.fd);
  }
  ::unlink(slice_path(backup_id).c_str());
  m_sessions.erase(it);

  if (m_sessions.empty()) {
    m_active.store(false, std::memory_order_release);
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

  const std::string path_str = (path != nullptr) ? path : "";

  if (path != nullptr) {
    ut::free(path);
  }

  std::lock_guard<std::mutex> guard(m_mutex);

  /* Fan the event out to every registered session. Each keeps its own
  sequence counter, so a session's slice is a self-contained JSONL file. Build
  the record with rapidjson so the path is escaped correctly; the reader
  (XtraBackup) parses each line with rapidjson too. */
  for (auto &kv : m_sessions) {
    Session &session = kv.second;
    if (session.broken) {
      continue;
    }
    ++session.seq;

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("seq");
    w.Uint64(session.seq);
    w.Key("ev");
    w.String(begin ? "BEGIN" : "END");
    w.Key("type");
    w.String(type_str(type));
    w.Key("space");
    w.Uint(static_cast<unsigned>(space));
    w.Key("flags");
    w.Uint(space_flags == UINT32_UNDEFINED ? 0u : space_flags);
    w.Key("lsn");
    w.Uint64(lsn);
    w.Key("path");
    w.String(path_str.c_str(),
             static_cast<rapidjson::SizeType>(path_str.size()));
    w.EndObject();

    std::string line(sb.GetString(), sb.GetSize());
    line += '\n';

    append_record(session, line);
  }
}

void Backup_ddl_journal::log_point(Clone_notify::Type type, space_id_t space) {
  log(type, space, true);
  log(type, space, false);
}

void Backup_ddl_journal::deinit() {
  std::lock_guard<std::mutex> guard(m_mutex);
  for (auto &kv : m_sessions) {
    if (kv.second.fd >= 0) {
      ::close(kv.second.fd);
    }
    ::unlink(slice_path(kv.first).c_str());
  }
  m_sessions.clear();
  m_active.store(false, std::memory_order_release);
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
