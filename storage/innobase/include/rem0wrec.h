/*****************************************************************************
Copyright (c) 2021, 2022, Oracle and/or its affiliates.

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

/** @file include/rem0wrec.h
 Record manager wrapper declaration.

 After INSTANT ADD/DROP feture, fields index on logical record might not be
 same as field index on physical record. So this wrapper is implemented which
 translates logical index to physical index. And then functions of low level
 record manager (rem0lrec.h) are called with physical index of the field.

 Created 13/08/2021 Mayank Prasad
******************************************************************************/

#ifndef rem0wrec_h
#define rem0wrec_h

#include "rem/rec.h"

/** Get an offset to the nth data field in a record.
@param[in]   offsets  array returned by rec_get_offsets()
@param[in]   n        index of the field
@param[out]  len      length of the field; UNIV_SQL_NULL if SQL null;
                      UNIV_SQL_ADD_COL_DEFAULT if it's default value and no
                      value inlined
@return offset from the origin of rec */
static inline ulint rec_get_nth_field_offs_low(const ulint *offsets, ulint n,
                                               ulint *len) {
  ulint offs;
  ulint length;
  ut_ad(n < rec_offs_n_fields(offsets));
  ut_ad(len);

  if (n == 0) {
    offs = 0;
  } else {
    offs = rec_offs_base(offsets)[n] & REC_OFFS_MASK;
  }

  length = rec_offs_base(offsets)[1 + n];

  if (length & REC_OFFS_SQL_NULL) {
    length = UNIV_SQL_NULL;
  } else if (length & REC_OFFS_DEFAULT) {
    length = UNIV_SQL_ADD_COL_DEFAULT;
  } else if (length & REC_OFFS_DROP) {
    length = UNIV_SQL_INSTANT_DROP_COL;
  } else {
    length &= REC_OFFS_MASK;
    length -= offs;
  }

  *len = length;
  return (offs);
}
/** The following function is used to get an offset to the nth data field in a
record.
@param[in]      index   record descriptor
@param[in]      offsets array returned by rec_get_offsets()
@param[in]      n       index of the field
@param[out]     len     length of the field; UNIV_SQL_NULL if SQL null;
                        UNIV_SQL_ADD_COL_DEFAULT if it's default value and no
                        value inlined
@return offset from the origin of rec */
static inline ulint rec_get_nth_field_offs(const dict_index_t *index,
                                           const ulint *offsets, ulint n,
                                           ulint *len) {
  if (index && index->has_row_versions()) {
    n = index->get_field_off_pos(n);
  }

  return rec_get_nth_field_offs_low(offsets, n, len);
}
/** Gets the value of the specified field in the record.
@param[in]      index   record descriptor
@param[in]      rec     physical record
@param[in]      offsets array returned by rec_get_offsets()
@param[in]      n       index of the field
@param[out]     len     length of the field, UNIV_SQL_NULL if SQL null
@return value of the field */
static inline byte *rec_get_nth_field(const dict_index_t *index,
                                      const rec_t *rec, const ulint *offsets,
                                      ulint n, ulint *len) {
  byte *field =
      const_cast<byte *>(rec) + rec_get_nth_field_offs(index, offsets, n, len);
  return (field);
}
/** Get the row version on an old style leaf page record.
This is only needed for table after instant ADD/DROP COLUMN.
@param[in]      rec             leaf page record
@return row version */
static inline uint8_t rec_get_instant_row_version_old(const rec_t *rec) {
  const byte *ptr = rec - (REC_N_OLD_EXTRA_BYTES + 1);
  uint8_t row_version = *ptr;
  ut_ad(row_version <= MAX_ROW_VERSION);

  return row_version;
}
/** Returns the offset of n - 1th field end if the record is stored in the
 2-byte offsets form. If the field is SQL null, the flag is ORed in the returned
 value.
 @return offset of the start of the PREVIOUS field, SQL null flag ORed */
static inline ulint rec_2_get_prev_field_end_info(
    const rec_t *rec, /*!< in: record */
    ulint n)          /*!< in: field index */
{
  ut_ad(!rec_get_1byte_offs_flag(rec));
  ut_ad(n <= rec_get_n_fields_old_raw(rec));

  uint32_t version_length = 0;
  if (rec_old_is_versioned(rec)) {
    version_length = 1;
  }

  return (
      mach_read_from_2(rec - (REC_N_OLD_EXTRA_BYTES + version_length + 2 * n)));
}
/** Returns the offset of n - 1th field end if the record is stored in the
 1-byte offsets form. If the field is SQL null, the flag is ORed in the returned
 value. This function and the 2-byte counterpart are defined here because the
 C-compiler was not able to sum negative and positive constant offsets, and
 warned of constant arithmetic overflow within the compiler.
 @return offset of the start of the PREVIOUS field, SQL null flag ORed */
static inline ulint rec_1_get_prev_field_end_info(
    const rec_t *rec, /*!< in: record */
    ulint n)          /*!< in: field index */
{
  ut_ad(rec_get_1byte_offs_flag(rec));
  ut_ad(n <= rec_get_n_fields_old_raw(rec));

  uint32_t version_length = 0;
  if (rec_old_is_versioned(rec)) {
    version_length = 1;
  }

  return (mach_read_from_1(rec - (REC_N_OLD_EXTRA_BYTES + version_length + n)));
}
static inline ulint rec_1_get_field_start_offs_low(const rec_t *rec, ulint n) {
  ut_ad(rec_get_1byte_offs_flag(rec));
  ut_ad(n <= rec_get_n_fields_old_raw(rec));

  if (n == 0) {
    return (0);
  }

  return (rec_1_get_prev_field_end_info(rec, n) & ~REC_1BYTE_SQL_NULL_MASK);
}
static inline ulint rec_2_get_field_start_offs_low(const rec_t *rec, ulint n) {
  ut_ad(!rec_get_1byte_offs_flag(rec));
  ut_ad(n <= rec_get_n_fields_old_raw(rec));

  if (n == 0) {
    return (0);
  }

  return (rec_2_get_prev_field_end_info(rec, n) &
          ~(REC_2BYTE_SQL_NULL_MASK | REC_2BYTE_EXTERN_MASK));
}

static inline ulint rec_1_get_field_end_info_low(const rec_t *rec, ulint n) {
  ut_ad(rec_get_1byte_offs_flag(rec));
  ut_ad(n < rec_get_n_fields_old_raw(rec));

  uint32_t version_size = 0;
  if (rec_old_is_versioned(rec)) {
    version_size = 1;
  }

  return (
      mach_read_from_1(rec - (REC_N_OLD_EXTRA_BYTES + version_size + n + 1)));
}

static inline ulint rec_2_get_field_end_info_low(const rec_t *rec, ulint n) {
  ut_ad(!rec_get_1byte_offs_flag(rec));
  ut_ad(n < rec_get_n_fields_old_raw(rec));

  uint32_t version_size = 0;
  if (rec_old_is_versioned(rec)) {
    version_size = 1;
  }

  return (mach_read_from_2(rec -
                           (REC_N_OLD_EXTRA_BYTES + version_size + 2 * n + 2)));
}
/** The following function is used to get the offset to the nth
data field in an old-style record.
@param[in]   rec  record
@param[in]   n    index of the field
@param[out]  len  length of the field; UNIV_SQL_NULL if SQL null;
@return offset to the field */
static inline ulint rec_get_nth_field_offs_old_low(const rec_t *rec, ulint n,
                                                   ulint *len) {
  ulint os;
  ulint next_os;

  ut_ad(len);
  ut_a(rec);
  ut_a(n < rec_get_n_fields_old_raw(rec));

  if (rec_get_1byte_offs_flag(rec)) {
    os = rec_1_get_field_start_offs_low(rec, n);

    next_os = rec_1_get_field_end_info_low(rec, n);

    if (next_os & REC_1BYTE_SQL_NULL_MASK) {
      *len = UNIV_SQL_NULL;

      return (os);
    }

    next_os = next_os & ~REC_1BYTE_SQL_NULL_MASK;
  } else {
    os = rec_2_get_field_start_offs_low(rec, n);

    next_os = rec_2_get_field_end_info_low(rec, n);

    if (next_os & REC_2BYTE_SQL_NULL_MASK) {
      *len = UNIV_SQL_NULL;

      return (os);
    }

    next_os = next_os & ~(REC_2BYTE_SQL_NULL_MASK | REC_2BYTE_EXTERN_MASK);
  }

  *len = next_os - os;

  ut_ad(*len < UNIV_PAGE_SIZE);

  return (os);
}

static inline ulint rec_get_nth_field_offs_old(const dict_index_t *index,
                                               const rec_t *rec, ulint n,
                                               ulint *len) {
  if (index) {
    ut_ad(!dict_table_is_comp(index->table));
    uint8_t version = rec_get_instant_row_version_old(rec);
    if (index->has_row_versions()) {
      n = index->get_field_phy_pos(n, version);
    }
  }

  return rec_get_nth_field_offs_old_low(rec, n, len);
}

static inline const byte *rec_get_nth_field_old(const dict_index_t *index,
                                                const rec_t *rec, ulint n,
                                                ulint *len) {
  const byte *field = rec + rec_get_nth_field_offs_old(index, rec, n, len);
  return (field);
}

static inline ulint rec_get_field_start_offs_low(const rec_t *rec, ulint n) {
  ut_ad(rec);
  ut_ad(n <= rec_get_n_fields_old_raw(rec));

  if (n == 0) {
    return (0);
  }

  if (rec_get_1byte_offs_flag(rec)) {
    return (rec_1_get_field_start_offs_low(rec, n));
  }

  return (rec_2_get_field_start_offs_low(rec, n));
}

/** Gets the physical size of an old-style field.
Also an SQL null may have a field of size > 0, if the data type is of a fixed
size.
@param[in]  rec   record
@param[in]  n     index of the field
@return field size in bytes */
static inline ulint rec_get_nth_field_size_low(const rec_t *rec, ulint n) {
  ulint os;
  ulint next_os;

  os = rec_get_field_start_offs_low(rec, n);
  next_os = rec_get_field_start_offs_low(rec, n + 1);

  ut_ad(next_os - os < UNIV_PAGE_SIZE);

  return (next_os - os);
}

/** Gets the physical size of an old-style field.
Also an SQL null may have a field of size > 0, if the data type is of a fixed
size.
@param[in]      index   record descriptor
@param[in]      rec     record
@param[in]      n       index of the field
@return field size in bytes */
[[nodiscard]] static inline ulint rec_get_nth_field_size(
    const dict_index_t *index, const rec_t *rec, ulint n) {
  if (index) {
    ut_ad(!dict_table_is_comp(index->table));
    uint8_t version = rec_get_instant_row_version_old(rec);
    if (index->has_row_versions()) {
      n = index->get_field_phy_pos(n, version);
    }
  }

  return rec_get_nth_field_size_low(rec, n);
}

/** The following function is used to get the offset to the nth
data field in an old-style record.
@param[in]      index   record descriptor
@param[in]      rec     record
@param[in]      n       index of the field
@param[in]      len     length of the field;UNIV_SQL_NULL if SQL null
@return offset to the field */
ulint rec_get_nth_field_offs_old(const dict_index_t *index, const rec_t *rec,
                                 ulint n, ulint *len);

/** Returns nonzero if the extern bit is set in nth field of rec.
@param[in]  offsets  array returned by rec_get_offsets()
@param[in]  n        index of the field
@return nonzero if externally stored */
static inline ulint rec_offs_nth_extern_low(const ulint *offsets, ulint n) {
  ut_ad(rec_offs_validate(nullptr, nullptr, offsets));
  ut_ad(n < rec_offs_n_fields(offsets));
  return (rec_offs_base(offsets)[1 + n] & REC_OFFS_EXTERNAL);
}

/** Returns nonzero if the extern bit is set in nth field of rec.
@param[in]      index           record descriptor
@param[in]      offsets         array returned by rec_get_offsets()
@param[in]      n               nth field
@return nonzero if externally stored */
[[nodiscard]] static inline ulint rec_offs_nth_extern(const dict_index_t *index,
                                                      const ulint *offsets,
                                                      ulint n) {
  if (index && index->has_row_versions()) {
    n = index->get_field_off_pos(n);
  }

  return (rec_offs_nth_extern_low(offsets, n));
}

static inline ulint rec_offs_nth_sql_null_low(const ulint *offsets, ulint n) {
  ut_ad(rec_offs_validate(nullptr, nullptr, offsets));
  ut_ad(n < rec_offs_n_fields(offsets));
  return (rec_offs_base(offsets)[1 + n] & REC_OFFS_SQL_NULL);
}

/** Mark the nth field as externally stored.
@param[in]  offsets  array returned by rec_get_offsets()
@param[in]  n       index of the field */
static inline void rec_offs_make_nth_extern_low(ulint *offsets, const ulint n) {
  ut_ad(!rec_offs_nth_sql_null_low(offsets, n));
  rec_offs_base(offsets)[1 + n] |= REC_OFFS_EXTERNAL;
}

/** Mark the nth field as externally stored.
@param[in]      index           record descriptor
@param[in]      offsets         array returned by rec_get_offsets()
@param[in]      n               nth field */
static inline void rec_offs_make_nth_extern(dict_index_t *index, ulint *offsets,
                                            ulint n) {
  if (index && index->has_row_versions()) {
    n = index->get_field_off_pos(n);
  }

  rec_offs_make_nth_extern_low(offsets, n);
}

/** Returns nonzero if the SQL NULL bit is set in nth field of rec.
@param[in]      index           record descriptor
@param[in]      offsets         array returned by rec_get_offsets()
@param[in]      n               nth field
@return nonzero if SQL NULL */
[[nodiscard]] static inline ulint rec_offs_nth_sql_null(
    const dict_index_t *index, const ulint *offsets, ulint n) {
  if (index && index->has_row_versions()) {
    n = index->get_field_off_pos(n);
  }

  return (rec_offs_nth_sql_null_low(offsets, n));
}

/** Returns nonzero if the default bit is set in nth field of rec.
@param[in]  offsets  array returned by rec_get_offsets()
@param[in]  n        index of the field
@return nonzero if default bit is set */
static inline ulint rec_offs_nth_default_low(const ulint *offsets, ulint n) {
  ut_ad(rec_offs_validate(nullptr, nullptr, offsets));
  ut_ad(n < rec_offs_n_fields(offsets));
  return (rec_offs_base(offsets)[1 + n] & REC_OFFS_DEFAULT);
}

/** Returns nonzero if the default bit is set in nth field of rec.
@param[in]      index           record descriptor
@param[in]      offsets         array returned by rec_get_offsets()
@param[in]      n               nth field
@return nonzero if default bit is set */
static inline ulint rec_offs_nth_default(const dict_index_t *index,
                                         const ulint *offsets, ulint n) {
  if (index && index->has_row_versions()) {
    n = index->get_field_off_pos(n);
  }

  return (rec_offs_nth_default_low(offsets, n));
}

/** Gets the physical size of a field.
@param[in]  offsets array returned by rec_get_offsets()
@param[in]  n       index of the field
@return length of field */
static inline ulint rec_offs_nth_size_low(const ulint *offsets, ulint n) {
  ut_ad(rec_offs_validate(nullptr, nullptr, offsets));
  ut_ad(n < rec_offs_n_fields(offsets));
  if (!n) {
    return (rec_offs_base(offsets)[1 + n] & REC_OFFS_MASK);
  }
  return ((rec_offs_base(offsets)[1 + n] - rec_offs_base(offsets)[n]) &
          REC_OFFS_MASK);
}

/** Gets the physical size of a field.
@param[in]      index           record descriptor
@param[in]      offsets         array returned by rec_get_offsets()
@param[in]      n               nth field
@return length of field */
[[nodiscard]] static inline ulint rec_offs_nth_size(const dict_index_t *index,
                                                    const ulint *offsets,
                                                    ulint n) {
  if (index && index->has_row_versions()) {
    n = index->get_field_off_pos(n);
  }

  return (rec_offs_nth_size_low(offsets, n));
}

/** Determine if the offsets are for a record in the new
 compact format.
 @return nonzero if compact format */
static inline bool rec_offs_comp(
    const ulint *offsets) /*!< in: array returned by rec_get_offsets() */
{
  ut_ad(rec_offs_validate(nullptr, nullptr, offsets));
  return (*rec_offs_base(offsets) & REC_OFFS_COMPACT) != 0;
}

static inline void rec_1_set_field_end_info_low(rec_t *rec, ulint n,
                                                ulint info) {
  ut_ad(rec_get_1byte_offs_flag(rec));
  ut_ad(n < rec_get_n_fields_old_raw(rec));

  uint32_t version_length = 0;
  if (rec_old_is_versioned(rec)) {
    version_length = 1;
  }

  mach_write_to_1(rec - (REC_N_OLD_EXTRA_BYTES + version_length + n + 1), info);
}

static inline void rec_2_set_field_end_info_low(rec_t *rec, ulint n,
                                                ulint info) {
  ut_ad(!rec_get_1byte_offs_flag(rec));
  ut_ad(n < rec_get_n_fields_old_raw(rec));

  uint32_t version_length = 0;
  if (rec_old_is_versioned(rec)) {
    version_length = 1;
  }

  mach_write_to_2(rec - (REC_N_OLD_EXTRA_BYTES + version_length + 2 * n + 2),
                  info);
}

static inline void rec_set_nth_field_null_bit_low(rec_t *rec, ulint i,
                                                  bool val) {
  ulint info;

  if (rec_get_1byte_offs_flag(rec)) {
    info = rec_1_get_field_end_info_low(rec, i);

    if (val) {
      info = info | REC_1BYTE_SQL_NULL_MASK;
    } else {
      info = info & ~REC_1BYTE_SQL_NULL_MASK;
    }

    rec_1_set_field_end_info_low(rec, i, info);

    return;
  }

  info = rec_2_get_field_end_info_low(rec, i);

  if (val) {
    info = info | REC_2BYTE_SQL_NULL_MASK;
  } else {
    info = info & ~REC_2BYTE_SQL_NULL_MASK;
  }

  rec_2_set_field_end_info_low(rec, i, info);
}
static inline void rec_set_nth_field_sql_null_low(rec_t *rec, ulint n) {
  ulint offset;

  offset = rec_get_field_start_offs_low(rec, n);

  data_write_sql_null(rec + offset, rec_get_nth_field_size_low(rec, n));

  rec_set_nth_field_null_bit_low(rec, n, true);
}

/** This is used to modify the value of an already existing field in a record.
The previous value must have exactly the same size as the new value. If len
is UNIV_SQL_NULL then the field is treated as an SQL null.
For records in ROW_FORMAT=COMPACT (new-style records), len must not be
UNIV_SQL_NULL unless the field already is SQL null.
@param[in]  rec      record
@param[in]  offsets  array returned by rec_get_offsets()
@param[in]  n        index of the field
@param[in]  data     pointer to the data if not SQL null
@param[in]  len      length of the data or UNIV_SQL_NULL */
static inline void rec_set_nth_field_low(rec_t *rec, const ulint *offsets,
                                         ulint n, const void *data, ulint len) {
  byte *data2;
  ulint len2;

  ut_ad(rec);
  ut_ad(rec_offs_validate(rec, nullptr, offsets));

  auto fn = [&](const ulint *offsets1, ulint n1) {
    ulint n_drop = 0;
    for (size_t i = 0; i < n1; i++) {
      ulint len1 = rec_offs_base(offsets1)[1 + i];
      if (len1 & REC_OFFS_DROP) {
        n_drop++;
      }
    }
    return n_drop;
  };

  if (len == UNIV_SQL_NULL) {
    if (!rec_offs_nth_sql_null_low(offsets, n)) {
      ut_a(!rec_offs_comp(offsets));
      ulint n_drop = rec_old_is_versioned(rec) ? fn(offsets, n) : 0;
      rec_set_nth_field_sql_null_low(rec, n - n_drop);
    }

    return;
  }

  ut_ad(!rec_offs_nth_default_low(offsets, n));

  /* nullptr for index as n is physical here */
  data2 = rec_get_nth_field(nullptr, rec, offsets, n, &len2);

  if (len2 == UNIV_SQL_NULL) {
    ut_ad(!rec_offs_comp(offsets));
    ulint n_drop = rec_old_is_versioned(rec) ? fn(offsets, n) : 0;
    rec_set_nth_field_null_bit_low(rec, n - n_drop, false);
    ut_ad(len == rec_get_nth_field_size_low(rec, n - n_drop));
  } else {
    ut_ad(len2 == len);
  }

  ut_memcpy(data2, data, len);
}
/** This is used to modify the value of an already existing field in a record.
The previous value must have exactly the same size as the new value. If len is
UNIV_SQL_NULL then the field is treated as an SQL null.
For records in ROW_FORMAT=COMPACT (new-style records), len must not be
UNIV_SQL_NULL unless the field already is SQL null.
@param[in]      index   record descriptor
@param[in]      rec     record
@param[in]      offsets array returned by rec_get_offsets()
@param[in]      n       index number of the field
@param[in]      len     length of the data or UNIV_SQL_NULL.
                        If not SQL null, must have the same length as the
                        previous value.
                        If SQL null, previous value must be SQL null.
@param[in]      data    pointer to the data if not SQL null */
static inline void rec_set_nth_field(const dict_index_t *index, rec_t *rec,
                                     const ulint *offsets, ulint n,
                                     const void *data, ulint len) {
  if (index && index->has_row_versions()) {
    n = index->get_field_off_pos(n);
  }

  rec_set_nth_field_low(rec, offsets, n, data, len);
}

/** Returns nonzero if the field is stored off-page.
@param[in]      index   index
@param[in]      rec     record
@param[in]      n       field index
@retval 0 if the field is stored in-page
@retval REC_2BYTE_EXTERN_MASK if the field is stored externally */
[[nodiscard]] static inline ulint rec_2_is_field_extern(
    const dict_index_t *index, const rec_t *rec, ulint n) {
  if (index) {
    ut_ad(!dict_table_is_comp(index->table));
    uint8_t version = rec_get_instant_row_version_old(rec);
    if (index->has_row_versions()) {
      n = index->get_field_phy_pos(n, version);
    }
  }

  return (rec_2_get_field_end_info_low(rec, n) & REC_2BYTE_EXTERN_MASK);
}

/** The following function returns the data size of an old-style physical
record, that is the sum of field lengths. SQL null fields are counted as length
0 fields. The value returned by the function is the distance from record origin
to record end in bytes.
@return size */
static inline ulint rec_get_data_size_old(const rec_t *rec) {
  ut_ad(rec);

  return (rec_get_field_start_offs_low(rec, rec_get_n_fields_old_raw(rec)));
}
#endif
