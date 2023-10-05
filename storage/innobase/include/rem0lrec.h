/*****************************************************************************

Copyright (c) 2021, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*****************************************************************************/

/** @file include/rem0lrec.h
 Record manager.
 This file contains low level functions which deals with physical index of
 fields in a physical record.

 After INSTANT ADD/DROP feture, fields index on logical record might not be
 same as field index on physical record. So a wrapper (rem0wrec.h) is
 implemented which translates logical index to physical index. And then
 functions of this file are called with physical index of the field.

 Created 13/08/2021 Mayank Prasad
*****************************************************************************/

#ifndef rem0lrec_h
#define rem0lrec_h

#include "rem0rec.h"

/** Set nth field value to SQL NULL.
@param[in,out]  rec  record
@param[in]      n    index of the field. */
static inline void rec_set_nth_field_sql_null_low(rec_t *rec, ulint n);

/** Returns nonzero if the SQL NULL bit is set in nth field of rec.
@param[in]  offsets  array returned by rec_get_offsets()
@param[in]  n        index of the field.
@return nonzero if SQL NULL */
static inline ulint rec_offs_nth_sql_null_low(const ulint *offsets, ulint n);

/** Read the offset of the start of a data field in the record. The start of an
SQL null field is the end offset of the previous non-null field, or 0, if none
exists. If n is the number of the last field + 1, then the end offset of the
last field is returned.
@param[in]  rec  record
@param[in]  n    index of the field
@return offset of the start of the field */
static inline ulint rec_get_field_start_offs_low(const rec_t *rec, ulint n);

/** Returns the offset of nth field start if the record is stored in the 1-byte
offsets form.
@param[in]  rec  record
@param[in]  n    index of the field
@return offset of the start of the field */
static inline ulint rec_1_get_field_start_offs_low(const rec_t *rec, ulint n);

/** Returns the offset of nth field end if the record is stored in the 1-byte
offsets form. If the field is SQL null, the flag is ORed in the returned value.
@param[in]  rec  record
@param[in]  n    index of the field
@return offset of the start of the field, SQL null flag ORed */
static inline ulint rec_1_get_field_end_info_low(const rec_t *rec, ulint n);

/** Sets the field end info for the nth field if the record is stored in the
1-byte format.
@param[in,out]  rec   record
@param[in]      n     index of the field
@param[in]      info  value to set */
static inline void rec_1_set_field_end_info_low(rec_t *rec, ulint n,
                                                ulint info);
/** Returns the offset of nth field start if the record is stored in the 2-byte
offsets form.
@param[in]  rec   record
@param[in]  n     index of the field
@return offset of the start of the field */
static inline ulint rec_2_get_field_start_offs_low(const rec_t *rec, ulint n);

/** Returns the offset of nth field end if the record is stored in the 2-byte
offsets form. If the field is SQL null, the flag is ORed in the returned value.
@param[in]  rec   record
@param[in]  n     index of the field
@return offset of the start of the field, SQL null flag and extern storage flag
ORed */
static inline ulint rec_2_get_field_end_info_low(const rec_t *rec, ulint n);

/** Sets the field end info for the nth field if the record is stored in the
2-byte format.
@param[in]  rec   record
@param[in]  n     index of the field
@param[out] info  end info */
static inline void rec_2_set_field_end_info_low(rec_t *rec, ulint n,
                                                ulint info);

/** Sets the value of the ith field SQL null bit of an old-style record.
@param[in]  rec   record
@param[in]  i     index of the field
@param[in]  val   value to set */
static inline void rec_set_nth_field_null_bit_low(rec_t *rec, ulint i,
                                                  bool val);

#endif
