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

/** @file clone/clone0undo.cc
Roll back the donor's open transactions on a table of a partial clone.

The staged undo tablespaces are as of the end LSN of the clone. Every undo
segment still ACTIVE or PREPARED there belongs to a transaction that was
open on the donor at that moment; the table pages carry its changes with
its transaction id. Those records are replayed, newest first, through
row_undo_ins() and row_undo_mod(), exactly as the rollback of a recovered
transaction does at startup. The undo records name the donor's table id, so
the table lookup of the undo code is redirected to the table being imported. */

#include "clone0undo.h"

#include <map>
#include <vector>

#include "clone0stage.h"
#include "dict0dd.h"
#include "fsp0fsp.h"
#include "fut0lst.h"
#include "mach0data.h"
#include "que0que.h"
#include "row0uins.h"
#include "row0umod.h"
#include "row0undo.h"
#include "trx0rec.h"
#include "trx0rseg.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0undo.h"
#include "ut0new.h"

#include "sql/sql_class.h"

namespace {

/** Report a problem with the staged undo.
@param[in]      what    description
@return DB_ERROR */
dberr_t undo_error(const std::string &what) {
  std::string mesg("Clone rollback: ");
  mesg.append(what);
  my_error(ER_INTERNAL_ERROR, MYF(0), mesg.c_str());
  return DB_ERROR;
}

/** One undo log (insert or update) of an open transaction. */
struct Undo_log {
  Clone_stage_file *m_file;
  bool m_insert;
  page_no_t m_hdr_page_no;
  ulint m_hdr_offset;
  page_no_t m_last_page_no;
};

/** An open transaction with its undo logs. */
struct Undo_trx {
  std::vector<Undo_log> m_logs;
};

/** Walks the records of one undo log backwards, newest first. */
class Undo_cursor {
 public:
  explicit Undo_cursor(const Undo_log &log) : m_log(log) {}

  ~Undo_cursor() { ut::aligned_free(m_frame); }

  /** Position on the last record of the log.
  @return error code */
  int start();

  /** Move to the previous record.
  @return error code */
  int prev();

  bool at_end() const { return m_rec == nullptr; }

  undo_no_t undo_no() const { return trx_undo_rec_get_undo_no(m_rec); }

  const byte *frame() const { return m_frame; }
  ulint offset() const { return m_rec - m_frame; }
  page_no_t page_no() const { return m_page_no; }
  const Undo_log &log() const { return m_log; }

 private:
  /** Read a page of the log and position on its last record, going back
  further while a page holds no record.
  @param[in]    page_no page to read
  @return error code */
  int read_page_last_rec(page_no_t page_no);

  const Undo_log &m_log;
  byte *m_frame{nullptr};
  page_no_t m_page_no{FIL_NULL};
  trx_undo_rec_t *m_rec{nullptr};
};

int Undo_cursor::read_page_last_rec(page_no_t page_no) {
  while (page_no != FIL_NULL) {
    auto err = m_log.m_file->read_page(page_no, m_frame);
    if (err != 0) {
      return err;
    }
    m_page_no = page_no;
    m_rec = trx_undo_page_get_last_rec(m_frame, m_log.m_hdr_page_no,
                                       m_log.m_hdr_offset);
    if (m_rec != nullptr) {
      return 0;
    }
    /* A page without records: continue on the previous one. */
    page_no = mach_read_from_4(m_frame + TRX_UNDO_PAGE_HDR +
                               TRX_UNDO_PAGE_NODE + FLST_PREV + FIL_ADDR_PAGE);
  }
  m_rec = nullptr;
  return 0;
}

int Undo_cursor::start() {
  /* The in-page helpers align the record pointer down to the page. */
  m_frame = static_cast<byte *>(
      ut::aligned_alloc(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX));
  if (m_frame == nullptr) {
    return ER_OUTOFMEMORY;
  }
  return read_page_last_rec(m_log.m_last_page_no);
}

int Undo_cursor::prev() {
  ut_ad(m_rec != nullptr);

  auto prev_rec = trx_undo_page_get_prev_rec(m_rec, m_log.m_hdr_page_no,
                                             m_log.m_hdr_offset);
  if (prev_rec != nullptr) {
    m_rec = prev_rec;
    return 0;
  }

  const page_no_t prev_page_no =
      mach_read_from_4(m_frame + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE +
                       FLST_PREV + FIL_ADDR_PAGE);
  return read_page_last_rec(prev_page_no);
}

/** Find the undo logs of the transactions that were open on the donor in
one staged undo tablespace.
@param[in,out]  file    staged undo tablespace, open
@param[in,out]  trxs    open transactions by id
@return error code */
int collect_open_trxs(Clone_stage_file &file,
                      std::map<trx_id_t, Undo_trx> &trxs) {
  auto frame = static_cast<byte *>(
      ut::aligned_alloc(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX));
  auto seg = static_cast<byte *>(
      ut::aligned_alloc(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX));
  if (frame == nullptr || seg == nullptr) {
    ut::aligned_free(frame);
    ut::aligned_free(seg);
    return ER_OUTOFMEMORY;
  }

  /* Page 3 lists the rollback segment header pages. */
  int err = file.read_page(FSP_RSEG_ARRAY_PAGE_NO, frame);
  std::vector<page_no_t> rseg_pages;

  if (err == 0) {
    /* The size field of the array is not maintained; unused slots are
    FIL_NULL (trx_rsegsf_get_page_no). */
    for (uint32_t i = 0; i < TRX_SYS_N_RSEGS; ++i) {
      const page_no_t page_no = mach_read_from_4(
          frame + RSEG_ARRAY_HEADER + RSEG_ARRAY_PAGES_OFFSET + 4 * i);
      if (page_no != FIL_NULL) {
        rseg_pages.push_back(page_no);
      }
    }
  }

  ulint n_segments = 0;
  std::map<ulint, ulint> states;

  /* Each rollback segment has slots pointing at undo segment headers. */
  for (auto rseg_page : rseg_pages) {
    err = file.read_page(rseg_page, frame);
    if (err != 0) {
      break;
    }

    for (ulint slot = 0; slot < TRX_RSEG_N_SLOTS && err == 0; ++slot) {
      const page_no_t seg_page = mach_read_from_4(
          frame + TRX_RSEG + TRX_RSEG_UNDO_SLOTS + TRX_RSEG_SLOT_SIZE * slot);
      if (seg_page == FIL_NULL) {
        continue;
      }

      err = file.read_page(seg_page, seg);
      if (err != 0) {
        break;
      }

      const auto state =
          mach_read_from_2(seg + TRX_UNDO_SEG_HDR + TRX_UNDO_STATE);
      ++n_segments;
      ++states[state];
      const bool prepared = state == TRX_UNDO_PREPARED_80028 ||
                            state == TRX_UNDO_PREPARED ||
                            state == TRX_UNDO_PREPARED_IN_TC;
      if (state != TRX_UNDO_ACTIVE && !prepared) {
        continue;
      }

      /* The last log header on the page owns the records of the segment. */
      const ulint hdr_offset =
          mach_read_from_2(seg + TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG);
      if (hdr_offset == 0) {
        continue;
      }

      Undo_log log;
      log.m_file = &file;
      log.m_insert = mach_read_from_2(seg + TRX_UNDO_PAGE_HDR +
                                      TRX_UNDO_PAGE_TYPE) == TRX_UNDO_INSERT;
      log.m_hdr_page_no = seg_page;
      log.m_hdr_offset = hdr_offset;
      log.m_last_page_no =
          mach_read_from_4(seg + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST +
                           FLST_LAST + FIL_ADDR_PAGE);

      const trx_id_t trx_id =
          mach_read_from_8(seg + hdr_offset + TRX_UNDO_TRX_ID);
      trxs[trx_id].m_logs.push_back(log);
    }
  }

  {
    std::string hist;
    for (const auto &st : states) {
      hist += " state " + std::to_string(st.first) + ": " +
              std::to_string(st.second);
    }
    ib::info(ER_IB_CLONE_START_STOP)
        << "Clone rollback: " << file.path() << ": " << rseg_pages.size()
        << " rollback segments, " << n_segments << " undo segments" << hist;
  }

  ut::aligned_free(frame);
  ut::aligned_free(seg);
  return err;
}

/** What the table lookup of the undo code gets instead of the dictionary. */
struct Table_map {
  dict_table_t *m_table;
  table_id_t m_donor_table_id;
};

/** Open the imported table for an undo record of the donor's table.
@param[in]      table_id        table id in the undo record
@param[in]      thd             server thread handle
@param[out]     mdl             metadata lock taken
@param[in]      arg             the Table_map
@return the table, or nullptr for any other table */
dict_table_t *map_table(table_id_t table_id, THD *thd, MDL_ticket **mdl,
                        void *arg) {
  auto map = static_cast<Table_map *>(arg);
  if (table_id != map->m_donor_table_id) {
    return nullptr;
  }
  return dd_table_open_on_id(map->m_table->id, thd, mdl, false, true);
}

/** Roll back one transaction's changes to the table.
@param[in]      thd             server thread handle
@param[in]      map             the table and its donor id
@param[in]      trx_id          the transaction on the donor
@param[in]      utrx            its undo logs
@param[in,out]  n_recs          records rolled back
@return error code */
dberr_t rollback_trx(THD *thd, Table_map &map, trx_id_t trx_id,
                     const Undo_trx &utrx, uint64_t &n_recs) {
  std::vector<std::unique_ptr<Undo_cursor>> cursors;
  undo_no_t max_undo_no = 0;

  for (const auto &log : utrx.m_logs) {
    auto cursor = std::make_unique<Undo_cursor>(log);
    if (cursor->start() != 0) {
      return DB_ERROR;
    }
    if (!cursor->at_end()) {
      max_undo_no = std::max(max_undo_no, cursor->undo_no());
      cursors.push_back(std::move(cursor));
    }
  }

  if (cursors.empty()) {
    return DB_SUCCESS;
  }

  /* A transaction object as trx_resurrect() would build: the donor's id,
  which the pages carry, and no undo segments of its own. */
  trx_t *trx = trx_allocate_for_background();
  trx->id = trx_id;
  trx->mysql_thd = thd;
  trx->is_recovered = true;
  trx->undo_no = max_undo_no + 1;
  trx->roll_limit = 0;
  trx->state.store(TRX_STATE_ACTIVE);
  ut_d(trx->in_rollback = true);

  mem_heap_t *heap = mem_heap_create(512, UT_LOCATION_HERE);
  que_fork_t *fork = que_fork_create(nullptr, nullptr, QUE_FORK_ROLLBACK, heap);
  fork->trx = trx;
  que_thr_t *thr = que_thr_create(fork, heap, nullptr);
  undo_node_t *node = row_undo_node_create(*trx, thr, heap, false);
  thr->child = node;

  node->open_table = map_table;
  node->open_table_arg = &map;
  node->no_history = true;

  dberr_t err = DB_SUCCESS;

  /* The two logs of a transaction are one stack ordered by undo number. */
  while (err == DB_SUCCESS) {
    Undo_cursor *top = nullptr;
    for (auto &cursor : cursors) {
      if (!cursor->at_end() &&
          (top == nullptr || cursor->undo_no() > top->undo_no())) {
        top = cursor.get();
      }
    }
    if (top == nullptr) {
      break;
    }

    const byte *rec = top->frame() + top->offset();

    if (trx_undo_rec_get_table_id(rec) == map.m_donor_table_id) {
      const bool is_insert = top->log().m_insert;

      node->undo_rec =
          trx_undo_rec_copy(top->frame(), top->offset(), node->heap);
      node->roll_ptr =
          trx_undo_build_roll_ptr(is_insert, top->log().m_file->space_id(),
                                  top->page_no(), top->offset());
      node->undo_no = top->undo_no();
      node->state = is_insert ? UNDO_NODE_INSERT : UNDO_NODE_MODIFY;
      trx->undo_no = node->undo_no;

      err = is_insert ? row_undo_ins(node, thr) : row_undo_mod(node, thr);

      node->pcur.close();
      mem_heap_empty(node->heap);
      ++n_recs;
    }

    if (err == DB_SUCCESS && top->prev() != 0) {
      err = DB_ERROR;
    }
  }

  mem_heap_free(node->heap);
  mem_heap_free(heap);

  /* Back to what trx_allocate_for_background() handed out. */
  ut_d(trx->in_rollback = false);
  trx->state.store(TRX_STATE_NOT_STARTED);
  trx->id = 0;
  trx->undo_no = 0;
  trx->roll_limit = 0;
  trx->is_recovered = false;
  trx->mysql_thd = nullptr;
  trx_free_for_background(trx);

  return err;
}

}  // namespace

dberr_t clone_rollback_staged(THD *thd, dict_table_t *table,
                              table_id_t donor_table_id,
                              const char *stage_dir) {
  /* The undo tablespaces are the files of the stage directory itself. */
  std::vector<std::unique_ptr<Clone_stage_file>> undo_files;
  std::map<trx_id_t, Undo_trx> trxs;

  MY_DIR *entries = my_dir(stage_dir, MYF(MY_WANT_STAT));
  if (entries == nullptr) {
    return undo_error(std::string("cannot read ") + stage_dir);
  }

  int err = 0;
  for (uint i = 0; i < entries->number_off_files && err == 0; ++i) {
    const auto &entry = entries->dir_entry[i];
    if (entry.mystat == nullptr || !MY_S_ISREG(entry.mystat->st_mode) ||
        static_cast<ulonglong>(entry.mystat->st_size) < UNIV_PAGE_SIZE ||
        static_cast<ulonglong>(entry.mystat->st_size) % UNIV_PAGE_SIZE != 0) {
      continue;
    }

    std::string path(stage_dir);
    path.push_back(OS_PATH_SEPARATOR);
    path.append(entry.name);

    auto file = std::make_unique<Clone_stage_file>(path);
    err = file->open(false);
    if (err != 0) {
      break;
    }
    if (!fsp_is_undo_tablespace(file->space_id())) {
      continue;
    }

    err = collect_open_trxs(*file, trxs);
    undo_files.push_back(std::move(file));
  }
  my_dirend(entries);

  if (err != 0) {
    return DB_ERROR;
  }

  Table_map map{table, donor_table_id};
  uint64_t n_recs = 0;
  dberr_t db_err = DB_SUCCESS;

  for (const auto &it : trxs) {
    db_err = rollback_trx(thd, map, it.first, it.second, n_recs);
    if (db_err != DB_SUCCESS) {
      break;
    }
  }

  if (db_err == DB_SUCCESS) {
    ib::info(ER_IB_CLONE_START_STOP)
        << "Clone rollback: table " << table->name << ": " << n_recs
        << " undo records of " << trxs.size() << " open transactions in "
        << undo_files.size() << " undo tablespaces";
  }
  return db_err;
}
