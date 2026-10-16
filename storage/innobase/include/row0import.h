/*****************************************************************************

Copyright (c) 2012, 2026, Oracle and/or its affiliates.

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

/** @file include/row0import.h
 Header file for import tablespace functions.

 Created 2012-02-08 by Sunny Bains
 *******************************************************/

#ifndef row0import_h
#define row0import_h

#include <string>
#include <vector>

#include "dict0types.h"
#include "univ.i"

// Forward declarations
class THD;
struct trx_t;
struct dict_table_t;
struct row_prebuilt_t;

/** Imports a tablespace. The space id in the .ibd file must match the space id
of the table in the data dictionary.
@param[in]      table           table
@param[in]      table_def       dd table
@param[in]      prebuilt        prebuilt struct in MySQL
@return error code or DB_SUCCESS */
[[nodiscard]] dberr_t row_import_for_mysql(dict_table_t *table,
                                           dd::Table *table_def,
                                           row_prebuilt_t *prebuilt);

/** One SDI record read from a tablespace file. */
struct Sdi_file_record {
  /** SDI type: SDI_TYPE_TABLE or SDI_TYPE_TABLESPACE */
  uint32_t type{};

  /** Id of the object in the server that wrote the file */
  uint64_t id{};

  /** The serialized dictionary information, uncompressed (JSON) */
  std::string json;
};

/** Read the SDI of a tablespace file that is not attached to this server.
The file is read directly, outside the buffer pool. If the tablespace is
encrypted, the key is read from the .cfp file next to it.
@param[in]   thd      session, for error messages
@param[in]   path     path of the .ibd file
@param[out]  records  the live SDI records of the file, in key order
@param[out]  space_flags  tablespace (FSP) flags of the file
@return DB_SUCCESS or error code; errors are reported to thd */
[[nodiscard]] dberr_t row_import_read_sdi_from_file(
    THD *thd, const char *path, std::vector<Sdi_file_record> &records,
    uint32_t *space_flags);

/** Attach the tablespace file of a table or partition created by IMPORT
TABLE FROM '<file>.ibd'. The table was created without a tablespace, like a
discarded one, and the file is already at the path the table uses. Its
definition is read from the SDI of the file, then the file is converted in
place and attached as for ALTER TABLE ... IMPORT TABLESPACE.
@param[in]      thd                 session
@param[in,out]  table               the new table or partition
@param[in,out]  table_def           definition of the new table
@param[in]      source_def          table definition read from the SDI
@param[in]      source_part         partition of source_def that table is,
                                    or nullptr
@param[in]      compression_option  COMPRESSION= option of the table
@return DB_SUCCESS or error code; errors are reported to thd */
[[nodiscard]] dberr_t row_import_attach(THD *thd, dict_table_t *table,
                                        dd::Table *table_def,
                                        const dd::Table &source_def,
                                        const dd::Partition *source_part,
                                        const char *compression_option);

#endif /* row0import_h */
