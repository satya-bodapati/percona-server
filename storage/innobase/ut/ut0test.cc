/*****************************************************************************

Copyright (c) 2020, 2026, Oracle and/or its affiliates.

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

#ifdef UNIV_DEBUG
#include "ut0test.h"

#include <map>
#include <set>

#include "btr0load.h"
#include "btr0mtib.h"
#include "buf0flu.h"
#include "ddl0impl-builder.h"
#include "dict0dd.h"
#include "dict0dict.h"
#include "fil0fil.h"
#include "page0zip.h"
#include "scope_guard.h"
#include "trx0trx.h"
#include "vec0aux.h"
#include "vec0dml.h"
#include "vec0index.h"
#include "vec0spann.h"

#define CALL_MEMBER_FN(object, ptrToMember) ((object).*(ptrToMember))

/** A macro to define a dispatch function or a command function.  They all
have the same signature.
@param[in]   func_  the function that is being declared. */
#define DISPATCH_FUNCTION_DEF(func_)                    \
  /* @param[in] tokens   the command line */            \
  /* @return RET_PASS on success, or the error code. */ \
  Ret_t func_(std::vector<std::string> &tokens) noexcept

namespace ib {

thread_local Tester tl_interpreter;

typedef Tester::Ret_t Ret_t;

#define DISPATCH(x) m_dispatch[#x] = &Tester::x

Tester::Tester() noexcept {
  /* Kindly keep the commands in alphabetical order. */
  DISPATCH(corrupt_ondisk_page0);
  DISPATCH(corrupt_ondisk_root_page);
  DISPATCH(count_page_type);
  DISPATCH(count_used_and_free);
  DISPATCH(dblwr_force_crash);
  DISPATCH(find_fil_page_lsn);
  DISPATCH(find_flush_sync_lsn);
  DISPATCH(find_ondisk_page_type);
  DISPATCH(find_root_page_no);
  DISPATCH(find_space_id);
  DISPATCH(find_tablespace_file_name);
  DISPATCH(find_tablespace_physical_page_size);
  DISPATCH(make_ondisk_root_page_zeroes);
  DISPATCH(make_page_dirty);
  DISPATCH(open_table);
  DISPATCH(vec_aux_insert_row);
  DISPATCH(vec_aux_update_row);
  DISPATCH(vec_aux_dump);
  DISPATCH(vec_knn);
  DISPATCH(spann_dump);
  DISPATCH(spann_stats);
  DISPATCH(print_dblwr_has_encrypted_pages);
  DISPATCH(print_tree);
}

void Tester::init() noexcept {
  TLOG("Tester::init()");
  std::ostringstream sout;
  m_thd = current_thd;
  XLOG("Initialization successfully completed");
  set_output(sout);
}

void scan_page_type(space_id_t space_id,
                    std::map<page_type_t, page_no_t> &result_map) {
  mtr_t mtr;

  bool found;
  const page_size_t page_size = fil_space_get_page_size(space_id, &found);
  ut_ad(found);
  fil_space_t *space = fil_space_acquire(space_id);

  for (page_no_t page_no = 0; page_no < space->size; ++page_no) {
    const page_id_t page_id(space_id, page_no);
    mtr_start(&mtr);
    buf_block_t *block =
        buf_page_get(page_id, page_size, RW_S_LATCH, UT_LOCATION_HERE, &mtr);
    page_type_t page_type = block->get_page_type();
    result_map[page_type]++;
    mtr_commit(&mtr);
  }

  fil_space_release(space);
}

DISPATCH_FUNCTION_DEF(Tester::count_page_type) {
  std::ostringstream sout;
  mtr_t mtr;
  std::map<page_type_t, page_no_t> result_map;

  TLOG("Tester::count_page_type()");
  ut_ad(tokens.size() == 2);
  ut_ad(tokens[0] == "count_page_type");

  const std::string space_name = tokens[1];

  const space_id_t space_id = fil_space_get_id_by_name(space_name.c_str());

  scan_page_type(space_id, result_map);

  page_no_t total = 0;
  for (auto it : result_map) {
    sout << fil_get_page_type_str(it.first) << ": " << it.second << std::endl;
    total += it.second;
  }
  sout << "Total: " << total << std::endl;
  set_output(sout);

  return RET_PASS;
}

DISPATCH_FUNCTION_DEF(Tester::count_used_and_free) {
  std::ostringstream sout;
  mtr_t mtr;
  std::map<page_type_t, page_no_t> result_map;

  TLOG("Tester::count_used_and_free()");
  ut_ad(tokens.size() == 2);
  ut_ad(tokens[0] == "count_used_and_free");

  const std::string space_name = tokens[1];

  const space_id_t space_id = fil_space_get_id_by_name(space_name.c_str());

  scan_page_type(space_id, result_map);

  page_no_t total = 0;
  for (auto it : result_map) {
    total += it.second;
  }
  const page_no_t pages_free = result_map[FIL_PAGE_TYPE_ALLOCATED];
  const page_no_t used = total - pages_free;
  const double fill_factor = (used / (double)total) * 100;
  const double free_factor = (pages_free / (double)total) * 100;
  sout << "Total= " << total << ", used=" << used << ", free=" << pages_free
       << std::endl;
  sout << "Fill factor= " << fill_factor << ", free factor= " << free_factor
       << std::endl;
  set_output(sout);

  return RET_PASS;
}

dict_table_t *Tester::is_table_open(
    const std::string &table_name) const noexcept {
  for (auto iter = m_open_tables.begin(); iter != m_open_tables.end(); ++iter) {
    dict_table_t *table = *iter;
    std::string tmp(table->name.m_name);
    if (table_name == tmp) {
      return table;
    }
  }

  return nullptr;
}

Ret_t Tester::open_table(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::open_table()");
  ut_ad(tokens[0] == "open_table");
  ut_ad(tokens.size() == 2);
  std::ostringstream sout;
  Ret_t ret;

  std::string table_name = tokens[1];
  dict_table_t *table = is_table_open(table_name);
  if (table == nullptr) {
    table = dd_table_open_on_name(nullptr, nullptr, table_name.c_str(), false,
                                  DICT_ERR_IGNORE_NONE);
  }
  if (table == nullptr) {
    XLOG("FAIL: Could not open table: " << table_name);
    ret = RET_FAIL;
  } else {
    m_open_tables.push_back(table);
    XLOG("PASS: Successfully opened table=" << table_name);
    ret = RET_PASS;
  }
  set_output(sout);
  return ret;
}

Ret_t Tester::find_space_id(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::find_space_id");
  ut_ad(tokens.size() == 2);

  std::string table_name = tokens[1];
  ut_ad(tokens[0] == "find_space_id");
  dict_table_t *table = is_table_open(table_name);

  ut_ad(table != nullptr);

  space_id_t space_id = table->space;

  dict_index_t *clust_index = table->first_index();
  page_no_t root_page_no = clust_index->page;

  TLOG("table_name=" << table_name << ", space_id=" << space_id
                     << ", root_page_no=" << root_page_no);

  {
    std::ostringstream sout;
    sout << space_id;
    set_output(sout);
  }

  return RET_PASS;
}

Ret_t Tester::find_root_page_no(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::find_root_page_no");
  ut_ad(tokens[0] == "find_root_page_no");
  ut_ad(tokens.size() == 2);

  std::string table_name = tokens[1];
  dict_table_t *table = is_table_open(table_name);

  ut_ad(table != nullptr);

  space_id_t space_id = table->space;

  dict_index_t *clust_index = table->first_index();
  page_no_t root_page_no = clust_index->page;

  TLOG("table_name=" << table_name << ", space_id=" << space_id
                     << ", root_page_no=" << root_page_no);

  {
    std::ostringstream sout;
    sout << root_page_no;
    set_output(sout);
  }

  return RET_PASS;
}

Ret_t Tester::find_fil_page_lsn(std::vector<std::string> &tokens) noexcept {
  ut_ad(tokens[0] == "find_fil_page_lsn");
  std::string space_id_str = tokens[1];
  std::string page_no_str = tokens[2];
  space_id_t space_id = std::stoul(space_id_str);
  space_id_t page_no = std::stoul(page_no_str);

  page_id_t page_id(space_id, page_no);
  bool found;
  page_size_t page_size = fil_space_get_page_size(space_id, &found);
  ut_ad(found);

  mtr_t mtr;
  mtr_start(&mtr);
  buf_block_t *block =
      buf_page_get(page_id, page_size, RW_X_LATCH, UT_LOCATION_HERE, &mtr);
  lsn_t newest_lsn = block->page.get_newest_lsn();
  mtr_commit(&mtr);

  std::ostringstream sout;
  sout << newest_lsn;
  set_output(sout);

  return RET_PASS;
}

Ret_t Tester::find_ondisk_page_type(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::find_ondisk_page_type()");
  if (tokens.size() != 3) {
    for (auto token : tokens) {
      TLOG("TOKEN: " << token);
    }
    ut_ad(tokens.size() == 3);
  }

  ut_ad(tokens[0] == "find_ondisk_page_type");
  const std::string space_id_str = tokens[1];
  const std::string page_no_str = tokens[2];

  const space_id_t space_id = std::stoul(space_id_str);
  space_id_t page_no = std::stoul(page_no_str);

  /* Calculate the offset here. */
  bool found;
  page_size_t page_size = fil_space_get_page_size(space_id, &found);
  ut_ad(found);

  const page_id_t page_id(space_id, page_no);

  /* The buffer into which file page header is read. */
  alignas(OS_FILE_LOG_BLOCK_SIZE) std::array<byte, OS_FILE_LOG_BLOCK_SIZE> buf;

  fil_space_t *space = fil_space_get(space_id);
  ut_ad(space != nullptr);

  const fil_node_t *node = space->get_file_node(&page_no);
  ut_ad(node->is_open);

  const os_offset_t offset = page_no * page_size.physical();

  /* When the space file is currently open we are not able to write to it
  directly on Windows. We must use the currently opened handle. Moreover,
  on Windows a file opened for the asynchronous access must be accessed only in
  a way that allows asynchronous completion of the request. This requires that
  the i/o buffer be aligned to OS block size and also its size divisible by OS
  block size. */

  IORequest read_io_type(IORequest::READ);
  const dberr_t err = os_file_read(read_io_type, node->name, node->handle,
                                   buf.data(), offset, OS_FILE_LOG_BLOCK_SIZE);
  if (err != DB_SUCCESS) {
    page_type_t page_type = fil_page_get_type(buf.data());
    TLOG("Could not read page_id=" << page_id << ", page_type=" << page_type
                                   << ", err=" << err);
    /* Since we do not pass encryption information in IORequest, if page is
    encrypted on disk, it cannot be decrypted by i/o layer. But the encrypted
    data will be available in the provided buffer. */

    if (err == DB_IO_DECRYPT_FAIL) {
      /* We expect this only for encrypted pages.  For this function, this error
      is OK because we will only read one header field and the header is not
      encrypted. */
      ut_ad(Encryption::is_encrypted_page(buf.data()));
    } else {
      return RET_FAIL;
    }
  }

  const byte *page = buf.data();
  const page_type_t type = fil_page_get_type(page);
  const char *page_type = fil_get_page_type_str(type);

  TLOG("page_type=" << type);
  TLOG("page_type=" << page_type);

  std::ostringstream sout;
  sout << page_type;
  set_output(sout);

  return RET_PASS;
}

/* ---------- PS-11300 vector aux DML test surface ---------- */

/** Locate the vector aux dict_table_t for a base table.
@return aux table (opened, caller closes) or nullptr */
/* MDL tickets for the pair opened by vec_test_open_aux; released by
vec_test_close_aux. After a restart nothing is dict-cached, so the opens
go through the DD layer, which requires real MDL. */
struct vec_test_tables_t {
  dict_table_t *base{nullptr};
  dict_table_t *aux{nullptr};
  MDL_ticket *base_mdl{nullptr};
  MDL_ticket *aux_mdl{nullptr};
};

static bool vec_test_open_aux(const std::string &base_name,
                              vec_test_tables_t &t, uint32_t *dims_out) {
  t.base = dd_table_open_on_name(current_thd, &t.base_mdl, base_name.c_str(),
                                 false, DICT_ERR_IGNORE_NONE);
  if (t.base == nullptr) {
    return false;
  }

  const dict_index_t *vec_index = nullptr;
  for (const dict_index_t *idx = t.base->first_index(); idx != nullptr;
       idx = idx->next()) {
    if (idx->is_vector()) {
      vec_index = idx;
      break;
    }
  }
  if (vec_index == nullptr) {
    dd_table_close(t.base, current_thd, &t.base_mdl, false);
    t.base = nullptr;
    return false;
  }

  if (dims_out != nullptr) {
    /* The dict col is BLOB-typed (Field_vector : Field_blob); its dict
    length is blob metadata, not the vector width. The debug commands
    derive dims from the data instead. */
    *dims_out = 0;
  }

  char aux_name[MAX_FULL_NAME_LEN];
  vec_aux_get_table_name(t.base, vec_index->id, Vec_index_type::HNSW, aux_name,
                         sizeof(aux_name));

  t.aux = dd_table_open_on_name(current_thd, &t.aux_mdl, aux_name, false,
                                DICT_ERR_IGNORE_NONE);
  if (t.aux == nullptr) {
    dd_table_close(t.base, current_thd, &t.base_mdl, false);
    t.base = nullptr;
    return false;
  }
  return true;
}

static void vec_test_close_aux(vec_test_tables_t &t) {
  if (t.aux != nullptr) {
    dd_table_close(t.aux, current_thd, &t.aux_mdl, false);
  }
  if (t.base != nullptr) {
    dd_table_close(t.base, current_thd, &t.base_mdl, false);
  }
}

/** Parse "1:2|3" into per-level neighbor label lists; "-" = empty. */
static bool vec_test_parse_nb(const std::string &spec,
                              std::vector<std::vector<std::size_t>> &out) {
  out.clear();
  if (spec == "-") {
    return false;
  }
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t bar = spec.find('|', pos);
    if (bar == std::string::npos) bar = spec.size();
    std::vector<std::size_t> level;
    size_t p = pos;
    while (p < bar) {
      size_t colon = spec.find(':', p);
      if (colon == std::string::npos || colon > bar) colon = bar;
      try {
        level.push_back(std::stoull(spec.substr(p, colon - p)));
      } catch (...) {
        return true;
      }
      p = colon + 1;
    }
    out.push_back(std::move(level));
    if (bar == spec.size()) break;
    pos = bar + 1;
  }
  return false;
}

Ret_t Tester::vec_aux_insert_row(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::vec_aux_insert_row()");
  ut_ad(tokens[0] == "vec_aux_insert_row");
  std::ostringstream sout;
  /* tokens: 0=cmd 1=db/table 2=id 3=level 4=vec_csv 5=nb_spec */
  if (tokens.size() != 6) {
    XLOG("FAIL: usage: vec_aux_insert_row db/table id level vec_csv nb_spec");
    set_output(sout);
    return RET_FAIL;
  }

  vec_test_tables_t tt;
  uint32_t dims = 0;
  if (!vec_test_open_aux(tokens[1], tt, &dims)) {
    XLOG("FAIL: no vector aux for " << tokens[1]);
    set_output(sout);
    return RET_FAIL;
  }
  dict_table_t *aux = tt.aux;
  auto guard = create_scope_guard([&]() { vec_test_close_aux(tt); });

  uint64_t id;
  int level;
  std::vector<float> vec;
  std::vector<std::vector<std::size_t>> neighbors;
  try {
    id = std::stoull(tokens[2]);
    level = std::stoi(tokens[3]);
    size_t pos = 0;
    const std::string &csv = tokens[4];
    while (pos <= csv.size()) {
      size_t comma = csv.find(',', pos);
      if (comma == std::string::npos) comma = csv.size();
      vec.push_back(std::stof(csv.substr(pos, comma - pos)));
      if (comma == csv.size()) break;
      pos = comma + 1;
    }
  } catch (...) {
    XLOG("FAIL: bad numeric argument");
    set_output(sout);
    return RET_FAIL;
  }
  if (vec.empty()) {
    XLOG("FAIL: empty vector");
    set_output(sout);
    return RET_FAIL;
  }
  dims = static_cast<uint32_t>(vec.size());
  if (vec_test_parse_nb(tokens[5], neighbors)) {
    XLOG("FAIL: bad nb_spec");
    set_output(sout);
    return RET_FAIL;
  }

  std::vector<byte> nb_blob;
  vec_aux_serialize_neighbors(neighbors, nb_blob);

  byte row_ref[8];
  mach_write_to_8(row_ref, id);

  vec_aux_row_t row;
  row.id = id;
  row.vec = vec.data();
  row.dims = dims;
  row.row_ref = row_ref;
  row.row_ref_len = sizeof(row_ref);
  row.level = level;
  row.neighbors = nb_blob.data();
  row.neighbors_len = nb_blob.size();

  trx_t *trx = trx_allocate_for_background();
  trx_start_internal(trx, UT_LOCATION_HERE);
  const dberr_t err = vec_aux_insert(trx, aux, row);
  trx_commit_for_mysql(trx);
  trx_free_for_background(trx);

  if (err != DB_SUCCESS) {
    XLOG("FAIL: vec_aux_insert err=" << static_cast<int>(err));
    set_output(sout);
    return RET_FAIL;
  }
  XLOG("PASS: inserted id=" << id);
  set_output(sout);
  return RET_PASS;
}

Ret_t Tester::vec_aux_update_row(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::vec_aux_update_row()");
  ut_ad(tokens[0] == "vec_aux_update_row");
  std::ostringstream sout;
  /* tokens: 0=cmd 1=db/table 2=id 3=nb_spec */
  if (tokens.size() != 4) {
    XLOG("FAIL: usage: vec_aux_update_row db/table id nb_spec");
    set_output(sout);
    return RET_FAIL;
  }

  vec_test_tables_t tt;
  if (!vec_test_open_aux(tokens[1], tt, nullptr)) {
    XLOG("FAIL: no vector aux for " << tokens[1]);
    set_output(sout);
    return RET_FAIL;
  }
  dict_table_t *aux = tt.aux;
  auto guard = create_scope_guard([&]() { vec_test_close_aux(tt); });

  uint64_t id;
  std::vector<std::vector<std::size_t>> neighbors;
  try {
    id = std::stoull(tokens[2]);
  } catch (...) {
    XLOG("FAIL: bad id");
    set_output(sout);
    return RET_FAIL;
  }
  if (vec_test_parse_nb(tokens[3], neighbors)) {
    XLOG("FAIL: bad nb_spec");
    set_output(sout);
    return RET_FAIL;
  }

  std::vector<byte> nb_blob;
  vec_aux_serialize_neighbors(neighbors, nb_blob);

  trx_t *trx = trx_allocate_for_background();
  trx_start_internal(trx, UT_LOCATION_HERE);
  const dberr_t err =
      vec_aux_update_neighbors(trx, aux, id, nb_blob.data(), nb_blob.size());
  trx_commit_for_mysql(trx);
  trx_free_for_background(trx);

  if (err != DB_SUCCESS) {
    XLOG("FAIL: vec_aux_update_neighbors err=" << static_cast<int>(err));
    set_output(sout);
    return RET_FAIL;
  }
  XLOG("PASS: updated id=" << id);
  set_output(sout);
  return RET_PASS;
}

Ret_t Tester::vec_aux_dump(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::vec_aux_dump()");
  ut_ad(tokens[0] == "vec_aux_dump");
  std::ostringstream sout;
  if (tokens.size() != 2) {
    XLOG("FAIL: usage: vec_aux_dump db/table");
    set_output(sout);
    return RET_FAIL;
  }

  vec_test_tables_t tt;
  uint32_t dims = 0;
  if (!vec_test_open_aux(tokens[1], tt, &dims)) {
    XLOG("FAIL: no vector aux for " << tokens[1]);
    set_output(sout);
    return RET_FAIL;
  }
  dict_table_t *aux = tt.aux;
  auto guard = create_scope_guard([&]() { vec_test_close_aux(tt); });

  std::vector<vec_loaded_row_t> rows;
  uint64_t raw_max_id = 0;
  bool saw_invisible = false;
  std::vector<uint64_t> dead;
  const dberr_t err =
      vec_aux_load_rows(aux, dims, &rows, &raw_max_id, &saw_invisible, &dead);
  if (err != DB_SUCCESS) {
    XLOG("FAIL: vec_aux_load_rows err=" << static_cast<int>(err));
    set_output(sout);
    return RET_FAIL;
  }

  sout << "count=" << rows.size() << " max_id=" << raw_max_id
       << " invisible=" << (saw_invisible ? 1 : 0);
  /* Tombstoned rows (row_ref NULL, skipped from the graph) — printed
  only when present so pre-DELETE recordings stay stable. */
  if (!dead.empty()) {
    sout << " dead=";
    for (size_t i = 0; i < dead.size(); ++i) {
      if (i != 0) sout << ":";
      sout << dead[i];
    }
  }
  sout << "\n";
  for (const auto &r : rows) {
    sout << "id=" << std::get<0>(r) << " level=" << std::get<1>(r) << " nb=";
    const auto &nbl = std::get<3>(r);
    for (size_t l = 0; l < nbl.size(); ++l) {
      if (l != 0) sout << "|";
      for (size_t i = 0; i < nbl[l].size(); ++i) {
        if (i != 0) sout << ":";
        sout << nbl[l][i];
      }
    }
    sout << "\n";
  }
  set_output(sout);
  return RET_PASS;
}

Ret_t Tester::vec_knn(std::vector<std::string> &tokens) noexcept {
  std::ostringstream sout;

  TLOG("Tester::vec_knn()");
  /* tokens: 0=cmd 1=db/table 2=vec_csv 3=k [4=ef] */
  if (tokens.size() != 4 && tokens.size() != 5) {
    XLOG("FAIL: usage: vec_knn db/table vec_csv k [ef]");
    set_output(sout);
    return RET_FAIL;
  }

  vec_test_tables_t tt;
  uint32_t dims = 0;
  if (!vec_test_open_aux(tokens[1], tt, &dims)) {
    XLOG("FAIL: no vector aux for " << tokens[1]);
    set_output(sout);
    return RET_FAIL;
  }
  auto guard = create_scope_guard([&]() { vec_test_close_aux(tt); });

  if (tt.base->vec == nullptr) {
    /* The companion is created at SQL open; a debug command must not
    guess dims/params. */
    XLOG("FAIL: graph not initialized; access " << tokens[1]
                                                << " via SQL first");
    set_output(sout);
    return RET_FAIL;
  }

  std::vector<float> qvec;
  {
    const std::string &csv = tokens[2];
    size_t pos = 0;
    while (pos <= csv.size()) {
      size_t comma = csv.find(',', pos);
      if (comma == std::string::npos) comma = csv.size();
      qvec.push_back(std::stof(csv.substr(pos, comma - pos)));
      if (comma == csv.size()) break;
      pos = comma + 1;
    }
  }
  if (qvec.size() != tt.base->vec->dims) {
    XLOG("FAIL: query dims " << qvec.size() << " != index dims "
                             << tt.base->vec->dims);
    set_output(sout);
    return RET_FAIL;
  }

  const size_t k = std::stoul(tokens[3]);
  const size_t ef = tokens.size() == 5 ? std::stoul(tokens[4]) : 0;

  std::vector<vec_knn_hit_t> hits;
  const dberr_t err = vec_index_for(tt.base)->knn(
      tt.base, current_thd, qvec.data(), qvec.size(), k, ef, &hits);
  if (err != DB_SUCCESS) {
    XLOG("FAIL: vec_knn_search err=" << static_cast<int>(err));
    set_output(sout);
    return RET_FAIL;
  }

  sout << "n=" << hits.size() << "\n";
  for (const auto &h : hits) {
    sout << "label=" << h.label << " dist=" << std::fixed
         << std::setprecision(2) << h.dist << " ref=";
    if (h.row_ref.size() == 8) {
      sout << mach_read_from_8(
          reinterpret_cast<const byte *>(h.row_ref.data()));
    } else {
      sout << "?";
    }
    sout << "\n";
  }
  set_output(sout);
  return RET_PASS;
}

/** Scan every non-delete-marked user record of a spann aux table's
clustered index, invoking cb(index, rec, offsets) on each. Latest
record versions — spann_dump runs on committed state (the build
committed with its ALTER; S3's write hooks are per-statement). */
template <typename Rec_fn>
static void spann_scan_table(dict_table_t *aux, Rec_fn cb) {
  dict_index_t *clust = aux->first_index();

  mem_heap_t *offset_heap = nullptr;

  mtr_t mtr;
  mtr_start(&mtr);

  btr_pcur_t pcur;
  pcur.open_at_side(true /* left */, clust, BTR_SEARCH_LEAF, true, 0, &mtr);

  ulint n_scanned = 0;

  while (pcur.move_to_next_user_rec(&mtr) == DB_SUCCESS) {
    const rec_t *rec = pcur.get_rec();

    ulint *offsets = rec_get_offsets(rec, clust, nullptr, ULINT_UNDEFINED,
                                     UT_LOCATION_HERE, &offset_heap);

    if (!rec_get_deleted_flag(rec, dict_table_is_comp(aux))) {
      cb(clust, rec, offsets);
    }

    /* Batch the mtr — see vec_aux_load_rows for the positioning
    subtlety. */
    if (++n_scanned % 512 == 0) {
      pcur.store_position(&mtr);
      mtr_commit(&mtr);
      mtr_start(&mtr);
      pcur.restore_position(BTR_SEARCH_LEAF, &mtr, UT_LOCATION_HERE);
    }
  }

  pcur.close();
  mtr_commit(&mtr);

  if (offset_heap != nullptr) {
    mem_heap_free(offset_heap);
  }
}

/** Everything spann_dump / spann_stats report about one index: the
on-disk truth from scans of the three aux tables, plus the runtime
view when one is open. */
struct spann_observed_t {
  std::map<uint64_t, ulint> list_sizes;
  std::set<uint64_t> labels;
  ulint n_postings{0};
  ulint n_heads{0};
  ulint n_dead{0};
  bool has_rt{false};
  vec_spann_stats_t rt;
};

/** Collect spann_observed_t for `base_name`; on failure writes the
FAIL message to `sout` and returns false. */
static bool spann_observe(const std::string &base_name,
                          std::ostringstream &sout, spann_observed_t &obs) {
  MDL_ticket *base_mdl = nullptr;
  dict_table_t *base = dd_table_open_on_name(
      current_thd, &base_mdl, base_name.c_str(), false, DICT_ERR_IGNORE_NONE);
  if (base == nullptr) {
    XLOG("FAIL: no such table " << base_name);
    return false;
  }
  auto base_guard = create_scope_guard(
      [&]() { dd_table_close(base, current_thd, &base_mdl, false); });

  const dict_index_t *vec_index = nullptr;
  for (const dict_index_t *idx = base->first_index(); idx != nullptr;
       idx = idx->next()) {
    if (idx->is_vector()) {
      vec_index = idx;
      break;
    }
  }
  if (vec_index == nullptr || vec_index->vec_type != Vec_index_type::SPANN) {
    XLOG("FAIL: no spann index on " << base_name);
    return false;
  }

  /* Open the three members of the aux set. */
  struct {
    const char *suffix;
    dict_table_t *table{nullptr};
    MDL_ticket *mdl{nullptr};
  } aux[3] = {{"", {}, {}}, {"_meta", {}, {}}, {"_dead", {}, {}}};

  auto aux_guard = create_scope_guard([&]() {
    for (auto &a : aux) {
      if (a.table != nullptr) {
        vec_aux_close_for_dml(a.table, current_thd, &a.mdl);
      }
    }
  });

  for (auto &a : aux) {
    a.table = vec_aux_open_for_dml(base, vec_index->id, Vec_index_type::SPANN,
                                   a.suffix, current_thd, &a.mdl);
    if (a.table == nullptr) {
      XLOG("FAIL: cannot open aux (suffix '" << a.suffix << "') for "
                                             << base_name);
      return false;
    }
  }

  /* Postings: per-list sizes + the distinct-label set. */
  spann_scan_table(aux[0].table, [&](const dict_index_t *clust,
                                     const rec_t *rec, const ulint *offsets) {
    ulint len;
    const byte *p = rec_get_nth_field(clust, rec, offsets, 0, &len);
    ut_a(len == 8);
    const uint64_t head_id = mach_read_from_8(p);
    p = rec_get_nth_field(clust, rec, offsets, 1, &len);
    ut_a(len == 8);
    obs.labels.insert(mach_read_from_8(p));
    ++obs.list_sizes[head_id];
    ++obs.n_postings;
  });

  /* _meta: head count (HEAD rows only). */
  spann_scan_table(aux[1].table, [&](const dict_index_t *clust,
                                     const rec_t *rec, const ulint *offsets) {
    ulint len;
    const byte *p = rec_get_nth_field(clust, rec, offsets, 0, &len);
    ut_a(len == 1);
    if (*p == VEC_SPANN_META_HEAD) {
      ++obs.n_heads;
    }
  });

  /* _dead: row count. */
  spann_scan_table(aux[2].table, [&](const dict_index_t *, const rec_t *,
                                     const ulint *) { ++obs.n_dead; });

  obs.has_rt = vec_spann_runtime_stats(base, &obs.rt);

  return true;
}

Ret_t Tester::spann_dump(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::spann_dump()");
  ut_ad(tokens[0] == "spann_dump");
  std::ostringstream sout;
  if (tokens.size() != 2) {
    XLOG("FAIL: usage: spann_dump db/table");
    set_output(sout);
    return RET_FAIL;
  }

  spann_observed_t obs;
  if (!spann_observe(tokens[1], sout, obs)) {
    set_output(sout);
    return RET_FAIL;
  }

  sout << "heads=" << obs.n_heads << " lists=" << obs.list_sizes.size()
       << " postings=" << obs.n_postings << " labels=" << obs.labels.size();
  if (!obs.labels.empty()) {
    sout << " label_min=" << *obs.labels.begin()
         << " label_max=" << *obs.labels.rbegin();
  }
  sout << " dead=" << obs.n_dead << "\n";
  for (const auto &ls : obs.list_sizes) {
    sout << "head=" << ls.first << " size=" << ls.second << "\n";
  }

  /* Runtime (RAM) view, when a loaded spann runtime is open: head
  count includes the implicit genesis head; per-list append counters
  are since-load deltas (the LIRE feed, S3). */
  if (obs.has_rt) {
    sout << "mem heads=" << obs.rt.n_heads;
    /* Printed only when nonzero so pre-S4 recordings stay stable. */
    if (obs.rt.dead_appends != 0) {
      sout << " dead_appends=" << obs.rt.dead_appends;
    }
    sout << "\n";
    for (const auto &la : obs.rt.list_appends) {
      sout << "mem head=" << la.first << " appends=" << la.second << "\n";
    }
  }

  set_output(sout);
  return RET_PASS;
}

Ret_t Tester::spann_stats(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::spann_stats()");
  ut_ad(tokens[0] == "spann_stats");
  std::ostringstream sout;
  if (tokens.size() != 2) {
    XLOG("FAIL: usage: spann_stats db/table");
    set_output(sout);
    return RET_FAIL;
  }

  spann_observed_t obs;
  if (!spann_observe(tokens[1], sout, obs)) {
    set_output(sout);
    return RET_FAIL;
  }

  /* Derived health gauges (S7): dead ratio = retired labels against
  the labels still present in postings (L2's GC trigger input); list
  balance = min/max/avg posting-list size (L1's split trigger input).
  Integer arithmetic — the output is for recorded tests. */
  ulint list_min = 0;
  ulint list_max = 0;
  for (const auto &ls : obs.list_sizes) {
    list_min = list_min == 0 ? ls.second : std::min(list_min, ls.second);
    list_max = std::max(list_max, ls.second);
  }
  const ulint list_avg =
      obs.list_sizes.empty() ? 0 : obs.n_postings / obs.list_sizes.size();
  const ulint dead_ratio_pct =
      obs.labels.empty() ? 0 : obs.n_dead * 100 / obs.labels.size();

  sout << "heads=" << obs.n_heads << " lists=" << obs.list_sizes.size()
       << " postings=" << obs.n_postings << " labels=" << obs.labels.size()
       << " dead=" << obs.n_dead << " dead_ratio_pct=" << dead_ratio_pct
       << " list_min=" << list_min << " list_max=" << list_max
       << " list_avg=" << list_avg << "\n";

  set_output(sout);
  return RET_PASS;
}

Ret_t Tester::find_tablespace_file_name(
    std::vector<std::string> &tokens) noexcept {
  std::ostringstream sout;

  TLOG("Tester::find_tablespace_file_name()");
  ut_ad(tokens.size() == 2);
  ut_ad(tokens[0] == "find_tablespace_file_name");

  std::string space_name = tokens[1];
  space_id_t space_id = fil_space_get_id_by_name(space_name.c_str());
  char *filename = fil_space_get_first_path(space_id);
  sout << filename;
  set_output(sout);
  ut::free(filename);
  return RET_PASS;
}

DISPATCH_FUNCTION_DEF(Tester::find_tablespace_physical_page_size) {
  std::ostringstream sout;

  TLOG("Tester::find_tablespace_physical_page_size()");
  ut_ad(tokens.size() == 2);
  ut_ad(tokens[0] == "find_tablespace_physical_page_size");

  std::string space_name = tokens[1];
  space_id_t space_id = fil_space_get_id_by_name(space_name.c_str());
  bool found;
  page_size_t page_size = fil_space_get_page_size(space_id, &found);
  ut_ad(found);
  sout << page_size.physical();
  set_output(sout);
  return RET_PASS;
}

DISPATCH_FUNCTION_DEF(Tester::make_ondisk_root_page_zeroes) {
  TLOG("Tester::make_ondisk_root_page_zeroes()");
  ut_ad(tokens.size() == 2);
  ut_ad(tokens[0] == "make_ondisk_root_page_zeroes");

  const std::string table_name = tokens[1];
  const dict_table_t *const table = is_table_open(table_name);

  ut_ad(table != nullptr);

  const dict_index_t *const clust_index = table->first_index();
  const page_no_t root_page_no = clust_index->page;
  const page_size_t page_size = dict_table_page_size(table);

  return clear_page_prefix(table->space, root_page_no, page_size.physical());
}

Ret_t Tester::corrupt_ondisk_root_page(
    std::vector<std::string> &tokens) noexcept {
  std::ostringstream sout;

  TLOG("Tester::corrupt_ondisk_root_page()");
  ut_ad(tokens.size() == 2);
  ut_ad(tokens[0] == "corrupt_ondisk_root_page");

  const std::string table_name = tokens[1];
  const dict_table_t *table = is_table_open(table_name);

  if (table == nullptr) {
    std::vector<std::string> cmd{"open_table", table_name};
    Ret_t st = open_table(cmd);

    if (st != RET_PASS) {
      XLOG("Failed to open table: " << table_name);
      set_output(sout);
      return st;
    }

    table = is_table_open(table_name);
  }

  if (table == nullptr) {
    XLOG("Failed to open table: " << table_name);
    set_output(sout);
    return RET_FAIL;
  }

  const dict_index_t *const clust_index = table->first_index();
  const page_no_t root_page_no = clust_index->page;

  return clear_page_prefix(table->space, root_page_no, FIL_PAGE_DATA);
}

Ret_t Tester::clear_page_prefix(const space_id_t space_id, page_no_t page_no,
                                const size_t prefix_length) {
  TLOG("Tester::clear_page_prefix()");
  ut::aligned_array_pointer<byte, OS_FILE_LOG_BLOCK_SIZE> mem;
  /* We read before write, as writes have to have length divisible by
  OS_FILE_LOG_BLOCK_SIZE thus we need to learn the content of non-zeroed suffix.
  Also, it's easier to spot errors during read than write, and this requires
  reading at least the FIL_PAGE_DATA first bytes */
  const auto buf_size =
      ut_uint64_align_up(prefix_length, OS_FILE_LOG_BLOCK_SIZE);

  mem.alloc(ut::Count(buf_size));
  const page_id_t page_id{space_id, page_no};
  fil_space_t *space = fil_space_get(space_id);

  // Note: this call adjusts page_no, so it becomes relative to the node
  fil_node_t *node = space->get_file_node(&page_no);
  ut_ad(node->is_open);

  const page_size_t page_size(space->flags);
  const size_t page_size_bytes = page_size.physical();
  ut_a(buf_size <= page_size_bytes);

  // Note: we use updated page_no here, as os_aio needs offset relative to node
  const os_offset_t offset = page_no * page_size_bytes;

  /* When the space file is currently open we are not able to write to it
  directly on Windows. We must use the currently opened handle. Moreover,
  on Windows a file opened for the AIO access must be accessed only by AIO
  methods. The AIO requires the operations to be aligned and with size
  divisible by OS block size, so we first read block of the first page, to
  corrupt it and write back. */

  byte *buf = mem;
  IORequest read_io_type(IORequest::READ);
  dberr_t err = os_file_read(read_io_type, node->name, node->handle, buf,
                             offset, buf_size);
  if (err != DB_SUCCESS) {
    page_type_t page_type = fil_page_get_type(buf);
    TLOG("Could not read page_id=" << page_id << ", page type=" << page_type
                                   << ", err=" << err);

    if (err == DB_IO_DECRYPT_FAIL) {
      /* We expect this only for encrypted pages. Since this function doesn't
      actually read (or use) the contents, this error is OK. */
      ut_ad(Encryption::is_encrypted_page(buf));
    } else {
      return RET_FAIL;
    }
  }

  ut_ad(prefix_length <= buf_size);
  memset(buf, 0x00, prefix_length);

  IORequest write_io_type(IORequest::WRITE);
  err = os_file_write(write_io_type, node->name, node->handle, buf, offset,
                      buf_size);
  if (err == DB_SUCCESS) {
    TLOG("Successfully zeroed prefix of page_id=" << page_id << ", prefix="
                                                  << prefix_length);
  } else {
    TLOG("Could not write zeros to page_id=" << page_id << ", err=" << err);
    return RET_FAIL;
  }
  return RET_PASS;
}

DISPATCH_FUNCTION_DEF(Tester::dblwr_force_crash) {
  TLOG("Tester::dblwr_force_crash()");
  ut_ad(tokens.size() == 3);
  ut_ad(tokens[0] == "dblwr_force_crash");

  std::string space_id_str = tokens[1];
  std::string page_no_str = tokens[2];
  space_id_t space_id = std::stoul(space_id_str);
  space_id_t page_no = std::stoul(page_no_str);

  page_id_t page_id(space_id, page_no);
  dblwr::Force_crash = page_id;
  return RET_PASS;
}

Ret_t Tester::corrupt_ondisk_page0(std::vector<std::string> &tokens) noexcept {
  TLOG("Tester::corrupt_ondisk_page0()");
  ut_ad(tokens.size() == 2);
  ut_ad(tokens[0] == "corrupt_ondisk_page0");

  const std::string table_name = tokens[1];
  const dict_table_t *const table = is_table_open(table_name);
  ut_ad(table != nullptr);

  return clear_page_prefix(table->space, 0, FIL_PAGE_DATA);
}

DISPATCH_FUNCTION_DEF(Tester::make_page_dirty) {
  TLOG("Tester::make_page_dirty()");
  ut_ad(tokens.size() == 3);
  ut_ad(tokens[0] == "make_page_dirty");

  mtr_t mtr;
  std::string space_id_str = tokens[1];
  std::string page_no_str = tokens[2];
  space_id_t space_id = std::stoul(space_id_str);
  space_id_t page_no = std::stoul(page_no_str);

  page_id_t page_id(space_id, page_no);

  fil_space_t *space = fil_space_acquire_silent(space_id);

  if (space == nullptr) {
    return RET_FAIL;
  }

  if (page_no > space->size) {
    fil_space_release(space);
    return RET_FAIL;
  }

  mtr.start();

  buf_block_t *block = buf_page_get(page_id, page_size_t(space->flags),
                                    RW_X_LATCH, UT_LOCATION_HERE, &mtr);

  if (block != nullptr) {
    byte *page = block->frame;
    page_type_t page_type = fil_page_get_type(page);

    /* Don't dirty a page that is not yet used. */
    if (page_type != FIL_PAGE_TYPE_ALLOCATED) {
      ib::info(ER_IB_MSG_574)
          << "Dirtying page: " << page_id
          << ", page_type=" << fil_get_page_type_str(page_type);

      auto page_zip = buf_block_get_page_zip(block);
      if (page_zip != nullptr && fil_page_index_page_check(page)) {
        mach_write_to_2(page + FIL_PAGE_TYPE, page_type);
        page_zip_write_header(page_zip, page + FIL_PAGE_TYPE, 2, &mtr);
      } else {
        mlog_write_ulint(page + FIL_PAGE_TYPE, page_type, MLOG_2BYTES, &mtr);
      }
    }
  }

  mtr.commit();
  mtr.wait_for_flush();

  fil_space_release(space);

  if (block != nullptr) {
    buf_flush_sync_all_buf_pools();
  }

  return RET_PASS;
}

DISPATCH_FUNCTION_DEF(Tester::print_tree) {
  TLOG("Tester::print_tree()");
  ut_ad(tokens.size() == 2);
  ut_ad(tokens[0] == "print_tree");

  std::string table_name = tokens[1];

  MDL_ticket *mdl;
  dict_table_t *table = dd_table_open_on_name(
      current_thd, &mdl, table_name.c_str(), false, DICT_ERR_IGNORE_NONE);
  if (table == nullptr) {
    TLOG("Could not open table: " << table_name);
    return RET_FAIL;
  }

  auto guard = create_scope_guard(
      [table, &mdl]() { dd_table_close(table, current_thd, &mdl, false); });

  const dict_index_t *clust_index = table->first_index();
  BFT::Callback cb;
  BFT bft(clust_index, cb);
  bft.traverse();
  TLOG(cb);
  return RET_PASS;
}

Ret_t Tester::print_dblwr_has_encrypted_pages(
    std::vector<std::string> &) noexcept {
  std::ostringstream sout;
  if (dblwr::has_encrypted_pages()) {
    std::string m1("Double write file has encrypted pages.");
    TLOG(m1);
    sout << m1;
  } else {
    std::string m1("Double write file has NO encrypted pages.");
    TLOG(m1);
    sout << m1;
  }

  set_output(sout);
  return RET_PASS;
}

void Tester::close_table(dict_table_t *table) noexcept {
  for (auto iter = m_open_tables.begin(); iter != m_open_tables.end(); ++iter) {
    dict_table_t *tmp = *iter;
    if (tmp == table) {
      m_open_tables.erase(iter);
      break;
    }
  }

  dict_table_close(table, false, false);
}

void Tester::destroy() noexcept {
  for (auto iter = m_open_tables.begin(); iter != m_open_tables.end(); ++iter) {
    dict_table_t *table = *iter;
    dict_table_close(table, false, false);
  }

  m_open_tables.clear();
}

Ret_t Tester::find_flush_sync_lsn(std::vector<std::string> &tokens) noexcept {
  ut_ad(tokens.size() == 1);
  ut_ad(tokens[0] == "find_flush_sync_lsn");

  std::ostringstream sout;
  sout << get_flush_sync_lsn();
  set_output(sout);
  return RET_PASS;
}

Ret_t Tester::run(const std::string &cmdline) noexcept {
  Ret_t ret = RET_PASS;
  std::vector<std::string> tokens;

  std::istringstream sin(cmdline);

  while (!sin.eof()) {
    std::string token;
    std::getline(sin, token, ' ');
    tokens.push_back(token);
  }

  std::string command = tokens[0];

  /* Save the current command token.  This will be the value of the
  innodb_interpreter variable. */
  m_command = command;
  m_thd = current_thd;

  if (command == "init") {
    /* do nothing. */
  } else if (command == "destroy") {
    destroy();
  } else if (command == "buf_flush_sync_all_buf_pools") {
    buf_flush_sync_all_buf_pools();
    TLOG("Executed buf_flush_sync_all_buf_pools()");
    ret = RET_PASS;
  } else if (command == "bulk_load_enable_slow_io") {
    Btree_multi::bulk_load_enable_slow_io_debug();
  } else if (command == "bulk_load_disable_slow_io") {
    Btree_multi::bulk_load_disable_slow_io_debug();
  } else {
    ret = RET_CMD_TBD;
  }

  /* Second approach. */
  if (ret == RET_CMD_TBD) {
    auto cmd = m_dispatch.find(command);

    if (cmd != m_dispatch.end()) {
      ret = CALL_MEMBER_FN(*this, cmd->second)(tokens);
    } else {
      ret = RET_CMD_TBD;
    }
  }

  if (ret == RET_CMD_TBD) {
    ret = RET_FAIL;
  }

  return ret;
}

void Tester::update_thd_variable() noexcept {
  char **output = thd_innodb_interpreter_output(m_thd);
  *output = const_cast<char *>(m_log.c_str());

  char **output2 = thd_innodb_interpreter(m_thd);
  *output2 = const_cast<char *>(m_command.c_str());
}

void Tester::set_output(const std::ostringstream &sout) noexcept {
  m_log = sout.str();
}

void Tester::set_output(const std::string &log) noexcept { m_log = log; }

void Tester::clear_output() noexcept { m_log = ""; }

void Tester::append_output(const std::string &log) noexcept { m_log += log; }

int interpreter_run(const char *command) noexcept {
  return (int)tl_interpreter.run(command);
}

}  // namespace ib

void ib_interpreter_update(MYSQL_THD thd [[maybe_unused]],
                           SYS_VAR *var [[maybe_unused]],
                           void *var_ptr [[maybe_unused]],
                           const void *save [[maybe_unused]]) {
  TLOG("ib_interpreter_update");

  /* Point the THD variables - innodb_interpreter and innodb_interpreter_output
  to the correct values. */
  ib::tl_interpreter.update_thd_variable();
}

int ib_interpreter_check(THD *thd [[maybe_unused]],
                         SYS_VAR *var [[maybe_unused]], void *save,
                         struct st_mysql_value *value) {
  TLOG("ib_interpreter_check");
  char buff[STRING_BUFFER_USUAL_SIZE];
  int len = sizeof(buff);

  ut_a(save != nullptr);
  ut_a(value != nullptr);

  const char *cmd = value->val_str(value, buff, &len);

  int ret = ib::interpreter_run(cmd ? cmd : "");

  TLOG("ib_interpreter_check() is returning: " << ret);
  *static_cast<const char **>(save) = cmd;
  return ret;
}
#endif /* UNIV_DEBUG */
