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
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/

/** @file clone/clone0redo.cc
Apply the redo of a partial clone to the staged tablespace files.

The staged redo is the archived redo of the donor, from the checkpoint taken
when the page copy started to the LSN at which the copy ended. It covers the
whole donor instance; only the records of the staged tablespaces are kept.
Each record is applied to the page read from the staged file when the page
is older than the record, exactly as crash recovery does, and the page LSN
is set to the end of the mini-transaction. */

#include "clone0redo.h"

#include <map>
#include <set>

#include "buf0buf.h"
#include "buf0flu.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "log0files_io.h"
#include "log0recv.h"
#include "log0types.h"
#include "mtr0log.h"
#include "mtr0mtr.h"
#include "os0enc.h"
#include "os0file.h"
#include "page0zip.h"
#include "srv0srv.h"
#include "ut0new.h"

#include "sql/sql_class.h"

namespace {

/** Memory for pending records before they are applied to the files. */
constexpr size_t REDO_BATCH_BYTES = 256 * 1024 * 1024;

/** Report an error about the staged redo.
@param[in]      what    description
@return ER_INTERNAL_ERROR */
int redo_error(const std::string &what) {
  std::string mesg("Clone redo apply: ");
  mesg.append(what);
  my_error(ER_INTERNAL_ERROR, MYF(0), mesg.c_str());
  return ER_INTERNAL_ERROR;
}

/** Report an error about a staged file.
@param[in]      path    file path
@return ER_CANT_OPEN_FILE */
int file_error(const std::string &path) {
  char errbuf[MYSYS_STRERROR_SIZE];
  my_error(ER_CANT_OPEN_FILE, MYF(0), path.c_str(), errno,
           my_strerror(errbuf, sizeof(errbuf), errno));
  return ER_CANT_OPEN_FILE;
}

/** A parsed redo record waiting to be applied. */
struct Redo_rec {
  mlog_id_t m_type;

  /** LSN range of the mini-transaction that wrote the record */
  lsn_t m_start_lsn;
  lsn_t m_end_lsn;

  std::vector<byte> m_body;
};

using Page_key = std::pair<space_id_t, page_no_t>;
using Page_recs = std::map<Page_key, std::vector<Redo_rec>>;

/** A staged tablespace file, read and written page by page. */
class Stage_file {
 public:
  explicit Stage_file(const Clone_stage_file &file)
      : m_path(file.m_path), m_space_id(file.m_space_id) {}

  ~Stage_file() { close(); }

  /** Open the file and read the tablespace header from page 0.
  @return error code */
  int open();

  void close() {
    if (m_open) {
      os_file_close(m_file);
      m_open = false;
    }
  }

  /** Grow the file with zero pages so that a page exists.
  @param[in]    page_no highest page number needed
  @return error code */
  int extend_to(page_no_t page_no);

  /** @return error code */
  int read_page(page_no_t page_no, byte *buf);

  /** @return error code */
  int write_page(page_no_t page_no, byte *buf);

  const page_size_t &page_size() const { return m_page_size; }
  space_id_t space_id() const { return m_space_id; }
  const std::string &path() const { return m_path; }

 private:
  IORequest request(ulint type, page_no_t page_no) {
    IORequest io_request(type);
    io_request.block_size(m_block_size);
    if (page_no != 0 && m_encryption.can_encrypt()) {
      io_request.get_encryption_info().set(m_encryption);
    }
    return io_request;
  }

  std::string m_path;
  space_id_t m_space_id;
  pfs_os_file_t m_file{};
  bool m_open{false};
  page_size_t m_page_size{0, 0, false};
  ulint m_block_size{0};
  page_no_t m_n_pages{0};
  Encryption_metadata m_encryption;
};

int Stage_file::open() {
  bool success;

  m_file = os_file_create_simple_no_error_handling(
      innodb_data_file_key, m_path.c_str(), OS_FILE_OPEN, OS_FILE_READ_WRITE,
      srv_read_only_mode, &success);

  if (!success) {
    return file_error(m_path);
  }
  m_open = true;

  os_file_stat_t stat_info;
  if (os_file_get_status(m_path.c_str(), &stat_info, false, false) !=
      DB_SUCCESS) {
    return file_error(m_path);
  }
  m_block_size = stat_info.block_size;

  auto page0 = static_cast<byte *>(
      ut::aligned_alloc(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX));
  if (page0 == nullptr) {
    return ER_OUTOFMEMORY;
  }

  IORequest io_request(IORequest::READ);
  io_request.block_size(m_block_size);
  auto err = os_file_read(io_request, m_path.c_str(), m_file, page0, 0,
                          UNIV_ZIP_SIZE_MIN);
  if (err != DB_SUCCESS) {
    ut::aligned_free(page0);
    return file_error(m_path);
  }

  const uint32_t flags = fsp_header_get_flags(page0);
  if (!fsp_flags_is_valid(flags)) {
    ut::aligned_free(page0);
    return redo_error("invalid tablespace flags in " + m_path);
  }
  m_page_size.copy_from(page_size_t(flags));

  if (m_page_size.is_compressed()) {
    ut::aligned_free(page0);
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "cloning a table with ROW_FORMAT=COMPRESSED");
    return ER_NOT_SUPPORTED_YET;
  }

  err = os_file_read(io_request, m_path.c_str(), m_file, page0, 0,
                     m_page_size.physical());
  if (err != DB_SUCCESS) {
    ut::aligned_free(page0);
    return file_error(m_path);
  }

  if (mach_read_from_4(page0 + FIL_PAGE_SPACE_ID) != m_space_id) {
    ut::aligned_free(page0);
    return redo_error("unexpected tablespace id in " + m_path);
  }

  /* The clone re-wrapped the tablespace key with this server's master
  key when it wrote page 0. */
  if (FSP_FLAGS_GET_ENCRYPTION(flags)) {
    byte key[Encryption::KEY_LEN];
    byte iv[Encryption::KEY_LEN];
    Encryption_key e_key{key, iv};

    if (!fsp_header_get_encryption_key(flags, e_key, page0)) {
      ut::aligned_free(page0);
      return redo_error("cannot read the encryption key of " + m_path);
    }
    Encryption::set_or_generate(Encryption::AES, key, iv, m_encryption);
  }
  ut::aligned_free(page0);

  const auto size = os_file_get_size(m_file);
  if (size == static_cast<os_offset_t>(~0)) {
    return file_error(m_path);
  }
  m_n_pages = static_cast<page_no_t>(size / m_page_size.physical());

  return 0;
}

int Stage_file::extend_to(page_no_t page_no) {
  if (page_no < m_n_pages) {
    return 0;
  }

  /* Pages allocated after the copy: as cloned-db recovery does, grow by
  whole extents. Their init record comes first in the redo. */
  page_no_t new_pages = page_no + 1;
  if (new_pages > FSP_EXTENT_SIZE) {
    new_pages = ut_calc_align(new_pages, FSP_EXTENT_SIZE);
  }

  const os_offset_t cur_size = os_offset_t{m_n_pages} * m_page_size.physical();
  const os_offset_t new_size = os_offset_t{new_pages} * m_page_size.physical();

  if (!os_file_set_size(m_path.c_str(), m_file, cur_size, new_size, false)) {
    return file_error(m_path);
  }
  m_n_pages = new_pages;
  return 0;
}

int Stage_file::read_page(page_no_t page_no, byte *buf) {
  auto io_request = request(IORequest::READ, page_no);
  const os_offset_t offset = os_offset_t{page_no} * m_page_size.physical();

  auto err = os_file_read(io_request, m_path.c_str(), m_file, buf, offset,
                          m_page_size.physical());
  return err == DB_SUCCESS ? 0 : file_error(m_path);
}

int Stage_file::write_page(page_no_t page_no, byte *buf) {
  auto io_request = request(IORequest::WRITE, page_no);
  const os_offset_t offset = os_offset_t{page_no} * m_page_size.physical();

  auto err = os_file_write(io_request, m_path.c_str(), m_file, buf, offset,
                           m_page_size.physical());
  return err == DB_SUCCESS ? 0 : file_error(m_path);
}

/** The staged redo files, read block by block from the checkpoint. */
class Redo_reader {
 public:
  explicit Redo_reader(const std::vector<std::string> &files)
      : m_files(files) {}

  ~Redo_reader() { close(); }

  /** Open the first file and find the checkpoint.
  @return error code */
  int open();

  /** Read the next block.
  @param[out]   block     OS_FILE_LOG_BLOCK_SIZE bytes, decrypted
  @param[out]   block_lsn LSN of the block
  @param[out]   data_len  bytes used in the block, header included
  @param[out]   last      true for the trailer block
  @return error code */
  int next_block(byte *block, lsn_t &block_lsn, uint32_t &data_len, bool &last);

  lsn_t checkpoint_lsn() const { return m_checkpoint_lsn; }

 private:
  void close() {
    if (m_open) {
      os_file_close(m_file);
      m_open = false;
    }
  }

  /** Open file m_cur and check its header against the LSN we expect.
  @return error code */
  int open_file();

  const std::vector<std::string> &m_files;
  size_t m_cur{0};
  pfs_os_file_t m_file{};
  bool m_open{false};
  os_offset_t m_size{0};
  os_offset_t m_offset{0};
  lsn_t m_block_lsn{0};
  lsn_t m_checkpoint_lsn{0};
  Encryption_metadata m_encryption;
};

int Redo_reader::open_file() {
  close();

  if (m_cur >= m_files.size()) {
    return redo_error("the redo ends before its trailer block");
  }
  const auto &path = m_files[m_cur];

  bool success;
  m_file = os_file_create_simple_no_error_handling(
      innodb_log_file_key, path.c_str(), OS_FILE_OPEN, OS_FILE_READ_ONLY,
      srv_read_only_mode, &success);
  if (!success) {
    return file_error(path);
  }
  m_open = true;

  m_size = os_file_get_size(m_file);
  if (m_size == static_cast<os_offset_t>(~0) || m_size < LOG_FILE_HDR_SIZE) {
    return redo_error("redo file too small: " + path);
  }

  byte header[LOG_FILE_HDR_SIZE];
  IORequest io_request(IORequest::READ);
  io_request.disable_compression();

  if (os_file_read(io_request, path.c_str(), m_file, header, 0,
                   LOG_FILE_HDR_SIZE) != DB_SUCCESS) {
    return file_error(path);
  }

  Log_file_header file_header;
  if (!log_file_header_deserialize(header, file_header)) {
    return redo_error("corrupt redo file header: " + path);
  }

  if (m_cur == 0) {
    Log_checkpoint_header checkpoint;
    if (!log_checkpoint_header_deserialize(header + LOG_CHECKPOINT_1,
                                           checkpoint) ||
        checkpoint.m_checkpoint_lsn == 0) {
      if (!log_checkpoint_header_deserialize(header + LOG_CHECKPOINT_2,
                                             checkpoint)) {
        return redo_error("corrupt checkpoint header: " + path);
      }
    }
    m_checkpoint_lsn = checkpoint.m_checkpoint_lsn;
    if (m_checkpoint_lsn < file_header.m_start_lsn) {
      return redo_error("checkpoint before the first redo block: " + path);
    }

    /* The clone re-wrapped the redo key with this server's master key. */
    const byte *info =
        header + LOG_ENCRYPTION + LOG_HEADER_ENCRYPTION_INFO_OFFSET;
    if (Encryption::is_encrypted_with_v3(info) &&
        !Encryption::decode_encryption_info(m_encryption, info, true)) {
      return redo_error("cannot read the redo encryption key: " + path);
    }

    m_block_lsn =
        ut_uint64_align_down(m_checkpoint_lsn, OS_FILE_LOG_BLOCK_SIZE);
    m_offset = LOG_FILE_HDR_SIZE + (m_block_lsn - file_header.m_start_lsn);

  } else {
    if (file_header.m_start_lsn != m_block_lsn) {
      return redo_error("redo files are not contiguous: " + path);
    }
    m_offset = LOG_FILE_HDR_SIZE;
  }

  return 0;
}

int Redo_reader::open() {
  m_cur = 0;
  return open_file();
}

int Redo_reader::next_block(byte *block, lsn_t &block_lsn, uint32_t &data_len,
                            bool &last) {
  if (m_offset + OS_FILE_LOG_BLOCK_SIZE > m_size) {
    ++m_cur;
    auto err = open_file();
    if (err != 0) {
      return err;
    }
  }

  IORequest io_request(IORequest::READ);
  io_request.disable_compression();

  if (os_file_read(io_request, m_files[m_cur].c_str(), m_file, block, m_offset,
                   OS_FILE_LOG_BLOCK_SIZE) != DB_SUCCESS) {
    return file_error(m_files[m_cur]);
  }

  if (log_block_get_encrypt_bit(block)) {
    if (!m_encryption.can_encrypt()) {
      return redo_error("encrypted redo block without a key");
    }
    Encryption encryption;
    encryption.set(m_encryption);
    if (encryption.decrypt_log(block, OS_FILE_LOG_BLOCK_SIZE) != DB_SUCCESS) {
      return redo_error("cannot decrypt a redo block");
    }
  }

  Log_data_block_header header;
  if (!log_data_block_header_deserialize(block, header)) {
    return redo_error("redo block checksum mismatch at LSN " +
                      std::to_string(m_block_lsn));
  }

  if (header.m_hdr_no != log_block_convert_lsn_to_hdr_no(m_block_lsn)) {
    return redo_error("unexpected redo block at LSN " +
                      std::to_string(m_block_lsn));
  }

  block_lsn = m_block_lsn;
  data_len = header.m_data_len;
  last = (data_len < OS_FILE_LOG_BLOCK_SIZE);

  m_offset += OS_FILE_LOG_BLOCK_SIZE;
  m_block_lsn += OS_FILE_LOG_BLOCK_SIZE;
  return 0;
}

/** Parse the staged redo into records of the staged tablespaces and apply
them. */
class Redo_applier {
 public:
  Redo_applier(THD *thd, const std::vector<Clone_stage_file> &data_files,
               const std::vector<std::string> &redo_files)
      : m_thd(thd), m_redo(redo_files) {
    for (const auto &file : data_files) {
      m_spaces.emplace(file.m_space_id, file);
    }
  }

  ~Redo_applier();

  /** @return error code */
  int run();

 private:
  /** Parse one record.
  @param[in]    ptr       record start
  @param[in]    end       end of parsable data
  @param[out]   type      record type, without the single record flag
  @param[out]   space_id  tablespace, for page records
  @param[out]   page_no   page, for page records
  @param[out]   body      record body, for page records
  @param[out]   len       record length, 0 if incomplete
  @return error code */
  int parse_rec(const byte *ptr, const byte *end, mlog_id_t &type,
                space_id_t &space_id, page_no_t &page_no, const byte *&body,
                size_t &len);

  /** Parse every complete mini-transaction in the buffer.
  @return error code */
  int parse_buffer();

  /** Keep a record of a staged page. */
  void add_rec(mlog_id_t type, space_id_t space_id, page_no_t page_no,
               const byte *body, size_t body_len, lsn_t start_lsn,
               lsn_t end_lsn);

  /** Apply all pending records to the files and forget them.
  @return error code */
  int apply_batch();

  /** Apply the records of one page.
  @return error code */
  int apply_page(Stage_file &file, page_no_t page_no,
                 const std::vector<Redo_rec> &recs);

  /** Apply one record to the private block.
  @return error code */
  int apply_rec(const Redo_rec &rec, Stage_file &file, page_no_t page_no,
                mtr_t &mtr);

  /** @return true if the type is a change to a page */
  static bool is_page_rec(mlog_id_t type) {
    switch (type) {
      case MLOG_FILE_CREATE:
      case MLOG_FILE_DELETE:
      case MLOG_FILE_RENAME:
      case MLOG_FILE_EXTEND:
      case MLOG_TABLE_DYNAMIC_META:
      case MLOG_INDEX_LOAD:
      case MLOG_MULTI_REC_END:
      case MLOG_DUMMY_RECORD:
      case MLOG_TEST:
        return false;
      default:
        return true;
    }
  }

  THD *m_thd;
  Redo_reader m_redo;
  std::map<space_id_t, Clone_stage_file> m_spaces;

  /** Parse buffer: redo bytes without block headers, from m_buf_lsn */
  std::vector<byte> m_buf;
  lsn_t m_buf_lsn{0};

  /** Pending records and their body bytes */
  Page_recs m_recs;
  size_t m_pending_bytes{0};

  /** Private block for the apply */
  buf_block_t *m_block{nullptr};
  byte *m_frame{nullptr};

  /** Statistics */
  uint64_t m_n_recs{0};
  uint64_t m_n_pages{0};
  lsn_t m_end_lsn{0};
};

Redo_applier::~Redo_applier() {
  if (m_block != nullptr) {
    rw_lock_free(&m_block->lock);
    ut_d(rw_lock_free(&m_block->debug_latch));
    mutex_free(&m_block->mutex);
    ut::free(m_block);
  }
  ut::aligned_free(m_frame);
}

int Redo_applier::parse_rec(const byte *ptr, const byte *end, mlog_id_t &type,
                            space_id_t &space_id, page_no_t &page_no,
                            const byte *&body, size_t &len) {
  len = 0;
  body = nullptr;
  type = static_cast<mlog_id_t>(*ptr & ~MLOG_SINGLE_REC_FLAG);

  const byte *new_ptr = nullptr;

  switch (type) {
    case MLOG_MULTI_REC_END:
    case MLOG_DUMMY_RECORD:
      len = 1;
      return 0;

    case MLOG_TABLE_DYNAMIC_META: {
      /* Recovery hands these to recv_sys->metadata_recover, which does
      not exist on a running server. Parse for the length only. */
      table_id_t id;
      uint64_t version;
      mlog_id_t dummy;
      new_ptr =
          mlog_parse_initial_dict_log_record(ptr, end, &dummy, &id, &version);
      if (new_ptr != nullptr) {
        MetadataRecover metadata;
        new_ptr = metadata.parseMetadataLog(id, version, new_ptr, end);
      }
      break;
    }

    case MLOG_FILE_CREATE:
    case MLOG_FILE_DELETE:
    case MLOG_FILE_RENAME:
    case MLOG_FILE_EXTEND:
    case MLOG_INDEX_LOAD:
    case MLOG_WRITE_STRING: {
      /* These touch fil_system or recv_sys when recovery parses them.
      Parse them here, where nothing is attached. */
      mlog_id_t dummy;
      new_ptr =
          mlog_parse_initial_log_record(ptr, end, &dummy, &space_id, &page_no);
      if (new_ptr == nullptr) {
        break;
      }
      body = new_ptr;
      const page_id_t page_id(space_id, page_no);
      const ulint parsed = new_ptr - ptr;

      switch (type) {
        case MLOG_FILE_CREATE:
          new_ptr =
              fil_tablespace_redo_create(new_ptr, end, page_id, parsed, true);
          break;
        case MLOG_FILE_DELETE:
          new_ptr =
              fil_tablespace_redo_delete(new_ptr, end, page_id, parsed, true);
          break;
        case MLOG_FILE_RENAME:
          new_ptr =
              fil_tablespace_redo_rename(new_ptr, end, page_id, parsed, true);
          break;
        case MLOG_FILE_EXTEND:
          new_ptr =
              fil_tablespace_redo_extend(new_ptr, end, page_id, parsed, true);
          break;
        case MLOG_INDEX_LOAD:
          new_ptr = (end < new_ptr + 8) ? nullptr : new_ptr + 8;
          break;
        default:
          new_ptr = mlog_parse_string(new_ptr, end, nullptr, nullptr);
          break;
      }
      break;
    }

    default: {
      const byte *rec_body = nullptr;
      mlog_id_t parsed_type;
      ut_ad(!recv_sys->found_corrupt_log);

      len = recv_parse_log_rec(&parsed_type, ptr, end, &space_id, &page_no,
                               &rec_body);
      if (recv_sys->found_corrupt_log) {
        recv_sys->found_corrupt_log = false;
        return redo_error("corrupt redo record at LSN " +
                          std::to_string(m_buf_lsn));
      }
      body = rec_body;
      return 0;
    }
  }

  if (new_ptr != nullptr) {
    len = new_ptr - ptr;
  }
  return 0;
}

void Redo_applier::add_rec(mlog_id_t type, space_id_t space_id,
                           page_no_t page_no, const byte *body, size_t body_len,
                           lsn_t start_lsn, lsn_t end_lsn) {
  if (!is_page_rec(type) || m_spaces.count(space_id) == 0) {
    return;
  }

  Redo_rec rec;
  rec.m_type = type;
  rec.m_start_lsn = start_lsn;
  rec.m_end_lsn = end_lsn;
  rec.m_body.assign(body, body + body_len);

  m_recs[Page_key(space_id, page_no)].push_back(std::move(rec));
  m_pending_bytes += body_len + sizeof(Redo_rec);
  ++m_n_recs;
}

int Redo_applier::parse_buffer() {
  const byte *begin = m_buf.data();
  const byte *end = begin + m_buf.size();
  const byte *ptr = begin;
  lsn_t lsn = m_buf_lsn;

  while (ptr < end) {
    mlog_id_t type;
    space_id_t space_id = 0;
    page_no_t page_no = 0;
    const byte *body;
    size_t len;

    if ((*ptr & MLOG_SINGLE_REC_FLAG) != 0) {
      auto err = parse_rec(ptr, end, type, space_id, page_no, body, len);
      if (err != 0) {
        return err;
      }
      if (len == 0) {
        break;
      }
      const lsn_t end_lsn = recv_calc_lsn_on_data_add(lsn, len);
      add_rec(type, space_id, page_no, body,
              (body == nullptr) ? 0 : static_cast<size_t>(ptr + len - body),
              lsn, end_lsn);
      ptr += len;
      lsn = end_lsn;
      continue;
    }

    /* A multi-record mini-transaction: complete only with its end marker.
    Nothing is kept until the whole group is in the buffer. */
    struct Parsed {
      mlog_id_t m_type;
      space_id_t m_space_id;
      page_no_t m_page_no;
      const byte *m_body;
      size_t m_body_len;
    };
    std::vector<Parsed> group;
    const byte *rec_ptr = ptr;
    bool complete = false;

    while (rec_ptr < end) {
      auto err = parse_rec(rec_ptr, end, type, space_id, page_no, body, len);
      if (err != 0) {
        return err;
      }
      if (len == 0) {
        break;
      }
      if ((*rec_ptr & MLOG_SINGLE_REC_FLAG) != 0) {
        return redo_error("single record inside a multi-record group");
      }
      const size_t body_len =
          (body == nullptr) ? 0 : static_cast<size_t>(rec_ptr + len - body);
      group.push_back({type, space_id, page_no, body, body_len});
      rec_ptr += len;

      if (type == MLOG_MULTI_REC_END) {
        complete = true;
        break;
      }
    }

    if (!complete) {
      break;
    }

    const lsn_t end_lsn = recv_calc_lsn_on_data_add(lsn, rec_ptr - ptr);
    for (const auto &rec : group) {
      add_rec(rec.m_type, rec.m_space_id, rec.m_page_no, rec.m_body,
              rec.m_body_len, lsn, end_lsn);
    }
    ptr = rec_ptr;
    lsn = end_lsn;
  }

  /* Drop what has been consumed. */
  m_buf.erase(m_buf.begin(), m_buf.begin() + (ptr - begin));
  m_buf_lsn = lsn;
  return 0;
}

int Redo_applier::apply_rec(const Redo_rec &rec, Stage_file &file,
                            page_no_t page_no, mtr_t &mtr) {
  /* A record without a body (MLOG_INIT_FILE_PAGE2) still needs a valid
  pointer: the apply functions return nullptr for failure. */
  static const byte no_body = 0;
  const byte *body = rec.m_body.empty() ? &no_body : rec.m_body.data();
  const byte *end = body + rec.m_body.size();
  page_t *page = m_block->frame;
  const byte *new_ptr = nullptr;

  switch (rec.m_type) {
    case MLOG_1BYTE:
    case MLOG_2BYTES:
    case MLOG_4BYTES:
    case MLOG_8BYTES:
      if (page_no == 0) {
        /* Recovery also updates the in-memory fil_space_t from page 0
        changes. There is none for a staged file. */
        new_ptr = mlog_parse_nbytes(rec.m_type, body, end, page, nullptr);
        break;
      }
      [[fallthrough]];

    default: {
      if (rec.m_type == MLOG_WRITE_STRING && page_no == 0 && end >= body + 4) {
        /* Encryption information on page 0 was re-wrapped by the clone;
        the donor's version must not overwrite it. */
        const ulint offset = mach_read_from_2(body);
        if (offset == fsp_header_get_encryption_offset(file.page_size())) {
          return 0;
        }
      }
      /* The record changes a page the same way crash recovery would. */
      recv_recovery_on_thread = true;
      new_ptr = recv_apply_log_rec_body(rec.m_type, body, end, file.space_id(),
                                        page_no, m_block, &mtr);
      recv_recovery_on_thread = false;
      break;
    }
  }

  if (new_ptr == nullptr) {
    return redo_error(
        "cannot apply " + std::string(get_mlog_string(rec.m_type)) + " (LSN " +
        std::to_string(rec.m_start_lsn) + "-" + std::to_string(rec.m_end_lsn) +
        ", " + std::to_string(rec.m_body.size()) + " bytes) to page " +
        std::to_string(page_no) + " (LSN " +
        std::to_string(mach_read_from_8(page + FIL_PAGE_LSN)) + ") of " +
        file.path());
  }
  return 0;
}

int Redo_applier::apply_page(Stage_file &file, page_no_t page_no,
                             const std::vector<Redo_rec> &recs) {
  auto err = file.extend_to(page_no);
  if (err != 0) {
    return err;
  }

  err = file.read_page(page_no, m_frame);
  if (err != 0) {
    return err;
  }

  const page_id_t page_id(file.space_id(), page_no);
  buf_block_set_file_page(m_block, page_id);
  m_block->page.size.copy_from(file.page_size());

  mtr_t mtr;
  mtr_start(&mtr);
  mtr.set_log_mode(MTR_LOG_NONE);

  /* Latch the private block as recovery latches a buffer pool block, so
  that the apply functions see the state they assert. */
  m_block->page.buf_fix_count.fetch_add(1);
  ut_d(rw_lock_s_lock(&m_block->debug_latch, UT_LOCATION_HERE));
  rw_lock_x_lock(&m_block->lock, UT_LOCATION_HERE);
  mtr.memo_push(m_block, MTR_MEMO_PAGE_X_FIX);

  lsn_t page_lsn = mach_read_from_8(m_frame + FIL_PAGE_LSN);
  lsn_t end_lsn = 0;
  bool modified = false;

  for (const auto &rec : recs) {
    if (rec.m_type == MLOG_INIT_FILE_PAGE) {
      page_lsn = 0;
      memset(m_frame + FIL_PAGE_LSN, 0, 8);
      memset(m_frame + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM, 0, 8);
    }

    if (rec.m_start_lsn < page_lsn) {
      /* The page already has this change. */
      continue;
    }

    err = apply_rec(rec, file, page_no, mtr);
    if (err != 0) {
      break;
    }
    modified = true;
    end_lsn = rec.m_end_lsn;
  }

  mtr_commit(&mtr);

  if (err == 0 && modified) {
    buf_flush_init_for_writing(m_block, m_frame, nullptr, end_lsn,
                               fsp_is_checksum_disabled(file.space_id()), true);
    err = file.write_page(page_no, m_frame);
    ++m_n_pages;
  }

  buf_block_set_state(m_block, BUF_BLOCK_NOT_USED);
  buf_block_set_state(m_block, BUF_BLOCK_READY_FOR_USE);
  return err;
}

int Redo_applier::apply_batch() {
  int err = 0;

  for (auto it = m_recs.begin(); it != m_recs.end() && err == 0;) {
    const space_id_t space_id = it->first.first;
    Stage_file file(m_spaces.at(space_id));

    err = file.open();

    for (; err == 0 && it != m_recs.end() && it->first.first == space_id;
         ++it) {
      err = apply_page(file, it->first.second, it->second);
    }
  }

  m_recs.clear();
  m_pending_bytes = 0;
  return err;
}

int Redo_applier::run() {
  auto err = m_redo.open();
  if (err != 0) {
    return err;
  }

  m_block = static_cast<buf_block_t *>(
      ut::zalloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, sizeof(buf_block_t)));
  m_frame = static_cast<byte *>(
      ut::aligned_alloc(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX));
  if (m_block == nullptr || m_frame == nullptr) {
    return ER_OUTOFMEMORY;
  }

  mutex_create(LATCH_ID_BUF_BLOCK_MUTEX, &m_block->mutex);
  rw_lock_create(PFS_NOT_INSTRUMENTED, &m_block->lock, LATCH_ID_BUF_BLOCK_LOCK);
  ut_d(rw_lock_create(PFS_NOT_INSTRUMENTED, &m_block->debug_latch,
                      LATCH_ID_BUF_BLOCK_DEBUG));
  m_block->lock.is_block_lock = true;
  m_block->latches_initialized.store(true);
  m_block->frame = m_frame;
  m_block->page.init_io_fix();
  m_block->page.buf_fix_count.store(1);
  m_block->page.state = BUF_BLOCK_READY_FOR_USE;
  page_zip_des_init(&m_block->page.zip);

  byte block[OS_FILE_LOG_BLOCK_SIZE];
  bool first = true;
  bool last = false;

  while (!last) {
    lsn_t block_lsn;
    uint32_t data_len;

    err = m_redo.next_block(block, block_lsn, data_len, last);
    if (err != 0) {
      return err;
    }

    if (thd_killed(m_thd)) {
      my_error(ER_QUERY_INTERRUPTED, MYF(0));
      return ER_QUERY_INTERRUPTED;
    }

    uint32_t start = LOG_BLOCK_HDR_SIZE;
    uint32_t stop = std::min<uint32_t>(
        data_len, OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE);

    if (first) {
      /* Start at the first record group, as recovery does. Records that
      end before the checkpoint are older than every staged page and
      the page LSN rule skips them. */
      const uint32_t first_rec = log_block_get_first_rec_group(block);
      if (first_rec == 0) {
        continue;
      }
      start = first_rec;
      m_buf_lsn = block_lsn + first_rec;
      first = false;
    }

    if (stop > start) {
      m_buf.insert(m_buf.end(), block + start, block + stop);
    }
    m_end_lsn = block_lsn + data_len;

    err = parse_buffer();
    if (err != 0) {
      return err;
    }

    if (m_pending_bytes >= REDO_BATCH_BYTES) {
      err = apply_batch();
      if (err != 0) {
        return err;
      }
    }
  }

  /* A mini-transaction cut by the end of the redo is not applied,
  exactly as recovery leaves it. */
  err = apply_batch();
  if (err != 0) {
    return err;
  }

  ib::info(ER_IB_CLONE_START_STOP)
      << "Clone Apply redo: " << m_n_recs << " records on " << m_n_pages
      << " pages, checkpoint LSN " << m_redo.checkpoint_lsn() << ", end LSN "
      << m_end_lsn;
  return 0;
}

}  // namespace

int clone_apply_staged_redo(THD *thd,
                            const std::vector<Clone_stage_file> &data_files,
                            const std::vector<std::string> &redo_files) {
  if (redo_files.empty()) {
    return redo_error("no redo files were staged");
  }

  Redo_applier applier(thd, data_files, redo_files);
  return applier.run();
}
