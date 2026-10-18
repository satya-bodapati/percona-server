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

/** @file include/clone0undo.h
Roll back the donor's open transactions on a table of a partial clone. */

#ifndef CLONE_UNDO_INCLUDE
#define CLONE_UNDO_INCLUDE

#include "dict0types.h"
#include "univ.i"

class THD;
struct dict_table_t;

/** Roll back, on an attached table of a partial clone, the changes of the
transactions that were open on the donor when the clone ended. Their undo
logs are read from the staged undo tablespaces and replayed with the
rollback functions of the server, in reverse order per transaction.
@param[in]      thd             server thread handle
@param[in]      table           the table, attached with its rows as the
                                donor had them
@param[in]      donor_table_id  id of the table on the donor, as the undo
                                records name it
@param[in]      stage_dir       staging directory of the clone
@return DB_SUCCESS or error code, reported with my_error() */
dberr_t clone_rollback_staged(THD *thd, dict_table_t *table,
                              table_id_t donor_table_id, const char *stage_dir);

#endif /* CLONE_UNDO_INCLUDE */
