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

/** @file include/clone0stage.h
A staged tablespace file of a partial clone, read and written page by page
without attaching it to the server. */

#ifndef CLONE_STAGE_INCLUDE
#define CLONE_STAGE_INCLUDE

#include <string>

#include "fil0fil.h"
#include "os0enc.h"
#include "os0file.h"
#include "page0size.h"
#include "univ.i"

/** A staged tablespace file. Page 0 gives the page size and, for an
encrypted tablespace, the key: the clone re-wrapped it with this server's
master key. */
class Clone_stage_file {
 public:
  /** @param[in] path       file path
  @param[in] space_id       tablespace id the file must have, or
                            SPACE_UNKNOWN to take it from page 0 */
  explicit Clone_stage_file(const std::string &path,
                            space_id_t space_id = SPACE_UNKNOWN)
      : m_path(path), m_space_id(space_id) {}

  ~Clone_stage_file() { close(); }

  Clone_stage_file(const Clone_stage_file &) = delete;
  Clone_stage_file &operator=(const Clone_stage_file &) = delete;

  /** Open the file and read the tablespace header from page 0.
  @param[in]    read_write      true to also write pages
  @return error code, reported with my_error() */
  int open(bool read_write);

  void close();

  /** Grow the file with zero pages so that a page exists.
  @param[in]    page_no highest page number needed
  @return error code */
  int extend_to(page_no_t page_no);

  /** @param[in] page_no page number
  @param[out]   buf     page, UNIV_PAGE_SIZE aligned
  @return error code */
  int read_page(page_no_t page_no, byte *buf);

  /** @param[in] page_no page number
  @param[in]    buf     page
  @return error code */
  int write_page(page_no_t page_no, byte *buf);

  const page_size_t &page_size() const { return m_page_size; }
  space_id_t space_id() const { return m_space_id; }
  page_no_t n_pages() const { return m_n_pages; }
  const std::string &path() const { return m_path; }

 private:
  IORequest request(ulint type, page_no_t page_no);

  std::string m_path;
  space_id_t m_space_id;
  pfs_os_file_t m_file{};
  bool m_open{false};
  page_size_t m_page_size{0, 0, false};
  ulint m_block_size{0};
  page_no_t m_n_pages{0};
  Encryption_metadata m_encryption;
};

/** Report an error about a staged file, from errno.
@param[in]      path    file path
@return ER_CANT_OPEN_FILE */
int clone_stage_file_error(const std::string &path);

#endif /* CLONE_STAGE_INCLUDE */
