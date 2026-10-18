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

/** @file include/clone0redo.h
Apply the redo of a partial clone to the staged tablespace files. */

#ifndef CLONE_REDO_INCLUDE
#define CLONE_REDO_INCLUDE

#include <string>
#include <vector>

#include "univ.i"

class THD;

/** One staged tablespace file of a partial clone. */
struct Clone_stage_entry {
  /** Path of the file under the staging directory */
  std::string m_path;

  /** Tablespace ID on the donor, as stored in the pages */
  space_id_t m_space_id;
};

/** Apply the staged redo to the staged tablespace files. The files are not
attached to the server: every page is read into a private block, the
records of its tablespace are applied as crash recovery would, and the page
is written back. Afterwards the files are consistent as of the end LSN of
the clone.
@param[in]      thd             server thread handle
@param[in]      data_files      staged tablespace files
@param[in]      redo_files      staged redo files, in order
@return error code */
int clone_apply_staged_redo(THD *thd,
                            const std::vector<Clone_stage_entry> &data_files,
                            const std::vector<std::string> &redo_files);

#endif /* CLONE_REDO_INCLUDE */
