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

/** @file include/row0quiesce.h

 Header file for tablespace quiesce functions.

 Created 2012-02-08 by Sunny Bains
 *******************************************************/

#ifndef row0quiesce_h
#define row0quiesce_h

#include "dict0types.h"
#include "fil0types.h"
#include "os0file.h"
#include "univ.i"

class THD;
namespace dd {
class Table;
}
struct trx_t;

/** The version number of the export meta-data text file. */
constexpr uint32_t IB_EXPORT_CFG_VERSION_V1 = 1;
/** The v2 .cfg has space flags written */
constexpr uint32_t IB_EXPORT_CFG_VERSION_V2 = 2;
/** The v3 .cfg writes instant column default values in metadata section. */
constexpr uint32_t IB_EXPORT_CFG_VERSION_V3 = 3;
/** The v4 .cfg has the is_ascending boolean written for each index column. */
constexpr uint32_t IB_EXPORT_CFG_VERSION_V4 = 4;
/** The v5 .cfg writes number of nullable column in table before first instant
 column. */
constexpr uint32_t IB_EXPORT_CFG_VERSION_V5 = 5;
/** The v6 .cfg writes the Compression::Type of the table. */
constexpr uint32_t IB_EXPORT_CFG_VERSION_V6 = 6;
/** The v7 .cfg has metadata of INSTANT DROP/ADD columns. */
constexpr uint32_t IB_EXPORT_CFG_VERSION_V7 = 7;
/** Future version used to test that the correct error message is returned. */
constexpr uint32_t IB_EXPORT_CFG_VERSION_V99 = 99;

/** Quiesce the tablespace that the table resides in.
@param[in] table Quiesce this table
@param[in,out] trx Transaction/session */
void row_quiesce_table_start(dict_table_t *table, trx_t *trx);

/** Set a table's quiesce state.
 @return DB_SUCCESS or error code. */
[[nodiscard]] dberr_t row_quiesce_set_state(
    dict_table_t *table, /*!< in: quiesce this table */
    ib_quiesce_t state,  /*!< in: quiesce state to set */
    trx_t *trx);         /*!< in/out: transaction */

/** Cleanup after table quiesce.
@param[in] table Quiesce this table
@param[in,out] trx Transaction/session */
void row_quiesce_table_complete(dict_table_t *table, trx_t *trx);

/** Facts about a table's tablespace and definition that the .cfg writer
needs. FLUSH TABLES ... FOR EXPORT takes them from the attached tablespace
and the data dictionary; a caller holding a table whose tablespace is not
attached to this server passes them explicitly. */
struct Cfg_write_source {
  /** Tablespace (FSP) flags */
  uint32_t space_flags{UINT32_UNDEFINED};

  /** Transparent page compression of the tablespace */
  Compression::Type compression{Compression::NONE};

  /** SDI index of the tablespace, or nullptr if it has none */
  const dict_index_t *sdi_index{nullptr};

  /** Root page of the SDI index in the tablespace */
  page_no_t sdi_root{FIL_NULL};

  /** Table definition holding INSTANT dropped columns, or nullptr to look
  the table up in the data dictionary by name */
  const dd::Table *dd_table{nullptr};
};

/** Write the .cfg contents of a table to an open file, in the format read
by ALTER TABLE ... IMPORT TABLESPACE.
@param[in]      table   table, with its index roots set
@param[in]      src     tablespace facts
@param[in,out]  file    file to write to
@param[in,out]  thd     session
@return DB_SUCCESS or error code */
[[nodiscard]] dberr_t row_quiesce_write_cfg_to_file(const dict_table_t *table,
                                                    const Cfg_write_source &src,
                                                    FILE *file, THD *thd);

#endif /* row0quiesce_h */
