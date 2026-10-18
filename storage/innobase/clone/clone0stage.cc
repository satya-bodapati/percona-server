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

/** @file clone/clone0stage.cc
A staged tablespace file of a partial clone. */

#include "clone0stage.h"

#include "fsp0fsp.h"
#include "mach0data.h"
#include "srv0srv.h"
#include "ut0new.h"

#include "sql/sql_class.h"

int clone_stage_file_error(const std::string &path) {
  char errbuf[MYSYS_STRERROR_SIZE];
  my_error(ER_CANT_OPEN_FILE, MYF(0), path.c_str(), errno,
           my_strerror(errbuf, sizeof(errbuf), errno));
  return ER_CANT_OPEN_FILE;
}

/** Report a problem with the contents of a staged file.
@param[in]      path    file path
@param[in]      what    description
@return ER_INTERNAL_ERROR */
static int stage_error(const std::string &path, const char *what) {
  std::string mesg("Clone staged file ");
  mesg.append(path).append(": ").append(what);
  my_error(ER_INTERNAL_ERROR, MYF(0), mesg.c_str());
  return ER_INTERNAL_ERROR;
}

IORequest Clone_stage_file::request(ulint type, page_no_t page_no) {
  IORequest io_request(type);
  io_request.block_size(m_block_size);
  if (page_no != 0 && m_encryption.can_encrypt()) {
    io_request.get_encryption_info().set(m_encryption);
  }
  return io_request;
}

void Clone_stage_file::close() {
  if (m_open) {
    os_file_close(m_file);
    m_open = false;
  }
}

int Clone_stage_file::open(bool read_write) {
  bool success;

  m_file = os_file_create_simple_no_error_handling(
      innodb_data_file_key, m_path.c_str(), OS_FILE_OPEN,
      read_write ? OS_FILE_READ_WRITE : OS_FILE_READ_ONLY, srv_read_only_mode,
      &success);

  if (!success) {
    return clone_stage_file_error(m_path);
  }
  m_open = true;

  os_file_stat_t stat_info;
  if (os_file_get_status(m_path.c_str(), &stat_info, false, false) !=
      DB_SUCCESS) {
    return clone_stage_file_error(m_path);
  }
  m_block_size = stat_info.block_size;

  auto page0 = static_cast<byte *>(
      ut::aligned_alloc(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX));
  if (page0 == nullptr) {
    return ER_OUTOFMEMORY;
  }

  int err = 0;
  IORequest io_request(IORequest::READ);
  io_request.block_size(m_block_size);

  if (os_file_read(io_request, m_path.c_str(), m_file, page0, 0,
                   UNIV_ZIP_SIZE_MIN) != DB_SUCCESS) {
    err = clone_stage_file_error(m_path);
    goto done;
  }

  {
    const uint32_t flags = fsp_header_get_flags(page0);
    if (!fsp_flags_is_valid(flags)) {
      err = stage_error(m_path, "invalid tablespace flags");
      goto done;
    }
    m_page_size.copy_from(page_size_t(flags));

    if (m_page_size.is_compressed()) {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "cloning a table with ROW_FORMAT=COMPRESSED");
      err = ER_NOT_SUPPORTED_YET;
      goto done;
    }

    if (os_file_read(io_request, m_path.c_str(), m_file, page0, 0,
                     m_page_size.physical()) != DB_SUCCESS) {
      err = clone_stage_file_error(m_path);
      goto done;
    }

    const space_id_t space_id = mach_read_from_4(page0 + FIL_PAGE_SPACE_ID);
    if (m_space_id == SPACE_UNKNOWN) {
      m_space_id = space_id;
    } else if (space_id != m_space_id) {
      err = stage_error(m_path, "unexpected tablespace id");
      goto done;
    }

    if (FSP_FLAGS_GET_ENCRYPTION(flags)) {
      byte key[Encryption::KEY_LEN];
      byte iv[Encryption::KEY_LEN];
      Encryption_key e_key{key, iv};

      if (!fsp_header_get_encryption_key(flags, e_key, page0)) {
        err = stage_error(m_path, "cannot read the encryption key");
        goto done;
      }
      Encryption::set_or_generate(Encryption::AES, key, iv, m_encryption);
    }

    const auto size = os_file_get_size(m_file);
    if (size == static_cast<os_offset_t>(~0)) {
      err = clone_stage_file_error(m_path);
      goto done;
    }
    m_n_pages = static_cast<page_no_t>(size / m_page_size.physical());
  }

done:
  ut::aligned_free(page0);
  return err;
}

int Clone_stage_file::extend_to(page_no_t page_no) {
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
    return clone_stage_file_error(m_path);
  }
  m_n_pages = new_pages;
  return 0;
}

int Clone_stage_file::read_page(page_no_t page_no, byte *buf) {
  if (page_no >= m_n_pages) {
    return stage_error(m_path, "page number beyond the file");
  }
  auto io_request = request(IORequest::READ, page_no);
  const os_offset_t offset = os_offset_t{page_no} * m_page_size.physical();

  if (os_file_read(io_request, m_path.c_str(), m_file, buf, offset,
                   m_page_size.physical()) != DB_SUCCESS) {
    return clone_stage_file_error(m_path);
  }
  return 0;
}

int Clone_stage_file::write_page(page_no_t page_no, byte *buf) {
  auto io_request = request(IORequest::WRITE, page_no);
  const os_offset_t offset = os_offset_t{page_no} * m_page_size.physical();

  if (os_file_write(io_request, m_path.c_str(), m_file, buf, offset,
                    m_page_size.physical()) != DB_SUCCESS) {
    return clone_stage_file_error(m_path);
  }
  return 0;
}
