/* Copyright (c) 2016, 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/sql_import.h"

#include <assert.h>
#include <sys/types.h>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "my_dir.h"  // MY_STAT
#include "my_inttypes.h"
#include "my_sys.h"  // my_realpath
#include "mysql/mysql_lex_string.h"
#include "prealloced_array.h"  // Prealloced_array
#include "sql/auth/auth_acls.h"
#include "sql/auth/auth_common.h"
#include "sql/dd/cache/dictionary_client.h"  // dd::cache::Dictionary_client::Auto_releaser
#include "sql/dd/dd.h"                       // dd::create_object
#include "sql/dd/dd_table.h"                 // dd::has_primary_key
#include "sql/dd/impl/sdi.h"                 // dd::deserialize
#include "sql/dd/impl/sdi_utils.h"  // dd::sdi_utils::handle_errors
#include "sql/dd/impl/types/object_table_definition_impl.h"  // fs_name_collation
#include "sql/dd/properties.h"                               // dd::Properties
#include "sql/dd/sdi_api.h"                // dd::sdi::Import_target
#include "sql/dd/sdi_file.h"               // dd::sdi_file::expand_pattern
#include "sql/dd/string_type.h"            // dd::String_type
#include "sql/dd/types/column.h"           // dd::Column
#include "sql/dd/types/foreign_key.h"      // dd::Foreign_key
#include "sql/dd/types/index.h"            // dd::Index
#include "sql/dd/types/partition.h"        // dd::Partition
#include "sql/dd/types/partition_index.h"  // dd::Partition_index
#include "sql/dd/types/schema.h"           // dd::Schema
#include "sql/dd/types/table.h"            // dd::Table
#include "sql/dd_sql_view.h"               // update_referencing_views_metadata
#include "sql/field.h"                     // COLUMN_FORMAT_TYPE_COMPRESSED
#include "sql/handler.h"                   // Sdi_from_file
#include "sql/mdl.h"                       // MDL_request
#include "sql/mysqld.h"                    // is_secure_file_path
#include "sql/psi_memory_key.h"            // key_memory_DD_import
#include "sql/sql_backup_lock.h"           // acquire_shared_backup_lock
#include "sql/sql_class.h"                 // THD
#include "sql/sql_db.h"                    // check_schema_readonly
#include "sql/sql_error.h"
#include "sql/sql_table.h"    // build_table_filename
#include "sql/srs_fetcher.h"  // Srs_fetcher
#include "sql/stateless_allocator.h"
#include "sql/strfunc.h"  // casedn
#include "sql/system_variables.h"
#include "sql/table.h"        // Table_ref
#include "sql/transaction.h"  // trans_rollback_stmt

namespace {

typedef Prealloced_array<dd::sdi::Import_target, 5> Targets_type;

}  // namespace

Sql_cmd_import_table::Sql_cmd_import_table(
    const Sdi_patterns_type &sdi_patterns)
    : m_sdi_patterns(sdi_patterns) {}

bool Sql_cmd_import_table::execute(THD *thd) {
  assert(!m_sdi_patterns.empty());

  /* IMPORT TABLE takes either SDI files of MyISAM tables or InnoDB
  tablespace files, never both in one statement. */
  size_t n_ibd = 0;
  for (auto &pattern : m_sdi_patterns) {
    if (pattern.length >= 4 &&
        my_strcasecmp(files_charset_info, pattern.str + pattern.length - 4,
                      ".ibd") == 0) {
      ++n_ibd;
    }
  }

  if (n_ibd != 0) {
    if (n_ibd != m_sdi_patterns.size()) {
      my_error(ER_WRONG_ARGUMENTS, MYF(0),
               "IMPORT TABLE (.sdi and .ibd files in one statement)");
      return true;
    }
    return execute_ibd(thd);
  }

  auto rbgrd = dd::sdi_utils::make_guard(thd, [&](THD *) {
    trans_rollback_stmt(thd);
    trans_rollback(thd);
  });

  // Need to keep this alive until after commit/rollback has been done
  const dd::cache::Dictionary_client::Auto_releaser ar{thd->dd_client()};

  if (check_access(thd, FILE_ACL, nullptr, nullptr, nullptr, false, false)) {
    return true;
  }

  // Convert supplied sdi patterns into path,in_datadir pairs
  dd::sdi_file::Paths_type paths{key_memory_DD_import};
  paths.reserve(m_sdi_patterns.size());
  for (auto &pattern : m_sdi_patterns) {
    if (thd->charset() == files_charset_info) {
      if (dd::sdi_file::expand_pattern(thd, pattern, &paths)) {
        return true;
      }
      continue;
    }

    LEX_STRING converted;
    if (thd->convert_string(&converted, files_charset_info, pattern.str,
                            pattern.length, thd->charset())) {
      return true;
    }

    if (dd::sdi_file::expand_pattern(thd, converted, &paths)) {
      return true;
    }
  }

  Targets_type targets{key_memory_DD_import};

  auto tgtgrd = dd::sdi_utils::make_guard(thd, [&](THD *) {
    for (auto &tgt : targets) {
      tgt.rollback();
    }
  });

  for (auto &p : paths) {
    // Move the path string from paths to avoid copy - paths is now
    // empty shell
    targets.emplace_back(std::move(p.first), p.second);
  }
  // Have a valid list of sdi files to import

  dd::String_type shared_buffer;
  MDL_request_list mdl_requests;
  for (auto &t : targets) {
    if (t.load(thd, &shared_buffer)) {
      return true;
    }

    if (check_privileges(thd, t)) {
      return true;
    }

    mdl_requests.push_front(mdl_request(t, thd->mem_root));
  }
  // Table objects and their schema names have been loaded, privileges
  // checked and EXCLUSIVE MDL requests for the tables been added to
  // mdl_requests.

  std::vector<dd::String_type> schema_names;
  schema_names.reserve(targets.size());
  for (auto &t : targets) {
    schema_names.push_back(*t.can_schema_name());
  }
  std::sort(schema_names.begin(), schema_names.end());
  auto uniq_end = std::unique(schema_names.begin(), schema_names.end());
  schema_names.erase(uniq_end, schema_names.end());

  for (auto &sn : schema_names) {
    MDL_request *r = new (thd->mem_root) MDL_request;
    MDL_REQUEST_INIT(r, MDL_key::SCHEMA, sn.c_str(), "",
                     MDL_INTENTION_EXCLUSIVE, MDL_TRANSACTION);
    mdl_requests.push_front(r);
  }

  MDL_request *mdl_request_for_backup_lock = new (thd->mem_root) MDL_request;
  MDL_REQUEST_INIT(mdl_request_for_backup_lock, MDL_key::BACKUP_LOCK, "", "",
                   MDL_INTENTION_EXCLUSIVE, MDL_TRANSACTION);
  mdl_requests.push_front(mdl_request_for_backup_lock);

  // If we cannot acquire protection against GRL, err out.
  if (thd->global_read_lock.can_acquire_protection()) return true;

  MDL_request *mdl_request_for_grl = new (thd->mem_root) MDL_request;
  MDL_REQUEST_INIT(mdl_request_for_grl, MDL_key::GLOBAL, "", "",
                   MDL_INTENTION_EXCLUSIVE, MDL_TRANSACTION);
  mdl_requests.push_front(mdl_request_for_grl);

  if (thd->mdl_context.acquire_locks(&mdl_requests,
                                     thd->variables.lock_wait_timeout)) {
    return true;
  }

  // Now when we have protection against concurrent change of read_only
  // option we can safely re-check its value.
  if (check_readonly(thd, true)) return true;

  // Loop over schema names and check schema read only.
  for (auto &sn : schema_names) {
    if (check_schema_readonly(thd, sn.c_str())) return true;
  }

  // Now we have MDL on all schemas and tables involved

  for (auto &t : targets) {
    if (t.store_in_dd(thd)) {
      return true;
    }
  }

  rbgrd.release();
  tgtgrd.release();

  // Downgrade failing delete_file errors to warning, and
  // allow the transaction to commit.
  dd::sdi_utils::handle_errors(
      thd,
      [](uint, const char *, Sql_condition::enum_severity_level *level,
         const char *) {
        (*level) = Sql_condition::SL_WARNING;
        return false;
      },
      [&]() {
        for (auto &tgt : targets) {
          (void)tgt.commit();
        }
        return false;
      });

  my_ok(thd);
  return trans_commit_stmt(thd) || trans_commit(thd);
}

enum_sql_command Sql_cmd_import_table::sql_command_code() const {
  return SQLCOM_IMPORT;
}

namespace {

/** One table of IMPORT TABLE FROM '<file>.ibd'. */
struct Ibd_target {
  /** The file holding the table SDI: the table's tablespace file, or the
  first partition's. */
  std::string sdi_file;

  /** Every named file that belongs to the table. */
  std::vector<std::string> files;

  /** The table definition read from the SDI. */
  std::unique_ptr<dd::Table> source;

  /** Schema name, as stored in the data dictionary. */
  dd::String_type schema;

  /** Table name, as stored in the data dictionary. */
  dd::String_type name;

  /** Schema name for MDL and privileges (lowercase if lctn=2). */
  dd::String_type mdl_schema;

  /** Table name for MDL and privileges (lowercase if lctn=2). */
  dd::String_type mdl_name;

  /** DATA DIRECTORY of the table, or empty if it is in the datadir. */
  std::string data_directory;

  /** Staging directory of a partial clone, or nullptr. */
  const char *clone_stage_dir{nullptr};
};

/** @return the real path of a file or directory, or an empty string if it
cannot be resolved
@param[in] path   path to resolve */
std::string real_path(const std::string &path) {
  char buf[FN_REFLEN];
  if (my_realpath(buf, path.c_str(), MYF(0)) != 0) {
    return std::string();
  }
  std::string result(buf);
  while (result.size() > 1 && result.back() == FN_LIBCHAR) {
    result.pop_back();
  }
  return result;
}

/** @return the directory part of a path, without the trailing separator
@param[in] path   path */
std::string dir_of(const std::string &path) {
  const size_t pos = path.find_last_of(FN_LIBCHAR);
  return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

/** @return the last component of a path
@param[in] path   path */
std::string base_of(const std::string &path) {
  const size_t pos = path.find_last_of(FN_LIBCHAR);
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

/** Compare two file names as the file system does.
@return true if they name the same file */
bool same_file_name(const std::string &a, const std::string &b) {
  return lower_case_file_system
             ? my_strcasecmp(files_charset_info, a.c_str(), b.c_str()) == 0
             : a == b;
}

/** Report a file of IMPORT TABLE FROM '<file>.ibd' that cannot be used.
@param[in] file   the file
@param[in] why    the reason
@return true */
bool wrong_ibd_file(const std::string &file, const char *why) {
  std::string msg("'");
  msg.append(file).append("' ").append(why);
  my_error(ER_TABLE_SCHEMA_MISMATCH, MYF(0), msg.c_str());
  return true;
}

/** Refuse a file that is the tablespace of a table that already exists,
before reading it: a live table's file can be read only partly flushed.
The table is found from the file's path, the way it was named.
@param[in]  thd   thread handle
@param[in]  file  path of the .ibd file
@return true if the table exists or the check failed, reported with
my_error() */
bool check_ibd_file_is_free(THD *thd, const std::string &file) {
  std::string base = base_of(file);
  base.resize(base.size() - 4);  // ".ibd"

  /* A partition file belongs to the table before "#p#". */
  size_t sep = base.find("#p#");
  if (sep == std::string::npos) sep = base.find("#P#");
  if (sep != std::string::npos) base.resize(sep);

  char schema[NAME_LEN + 1];
  char table[NAME_LEN + 1];
  filename_to_tablename(base_of(dir_of(file)).c_str(), schema, sizeof(schema),
                        true);
  filename_to_tablename(base.c_str(), table, sizeof(table), true);

  if (lower_case_table_names != 0) {
    my_casedn_str(files_charset_info, schema);
    my_casedn_str(files_charset_info, table);
  }

  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, schema, table, MDL_SHARED,
                   MDL_STATEMENT);
  if (thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout)) {
    return true;
  }

  const dd::Table *existing = nullptr;
  if (thd->dd_client()->acquire(schema, table, &existing)) {
    return true;
  }

  if (existing != nullptr) {
    my_error(ER_IMP_TABLE_ALREADY_EXISTS, MYF(0), schema, table);
    return true;
  }

  return false;
}

/** Read the table definition of a tablespace file and check that this
server can import it.
@param[in]      thd     thread handle
@param[in]      json    the table SDI read from the file
@param[in,out]  t       the target; sdi_file is set
@return true on error, reported with my_error() */
bool load_ibd_target(THD *thd, const std::string &json, Ibd_target *t) {
  t->source.reset(dd::create_object<dd::Table>());
  dd::String_type schema_in_sdi;

  /* No THD: the tablespace references in the SDI name tablespaces of the
  server that wrote the file, which need not exist here. The new table gets
  a tablespace of its own. */
  if (dd::deserialize(nullptr, dd::Sdi_type(json.data(), json.size()),
                      t->source.get(), &schema_in_sdi)) {
    if (!thd->is_error()) {
      wrong_ibd_file(t->sdi_file,
                     "holds a table definition that cannot be "
                     "read");
    }
    return true;
  }

  if (my_strcasecmp(system_charset_info, t->source->engine().c_str(),
                    "InnoDB") != 0) {
    return wrong_ibd_file(t->sdi_file, "is not an InnoDB table");
  }

  const dd::Table &source = *t->source;

  for (const dd::Column *column : source.columns()) {
    uint format = 0;
    if (column->options().exists("column_format") &&
        !column->options().get("column_format", &format) &&
        format == COLUMN_FORMAT_TYPE_COMPRESSED) {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "IMPORT TABLE of a table with compressed columns");
      return true;
    }
  }

  if (!dd::has_primary_key(source) && thd->variables.sql_require_primary_key) {
    my_error(ER_TABLE_WITHOUT_PK, MYF(0));
    return true;
  }

  const CHARSET_INFO *cs =
      dd::Object_table_definition_impl::fs_name_collation();

  t->schema = schema_in_sdi;
  t->name = t->source->name();

  if (lower_case_table_names == 1) {
    t->name = casedn(cs, t->name);
    t->schema = casedn(cs, t->schema);
    t->source->set_name(t->name);
  }

  t->mdl_schema = t->schema;
  t->mdl_name = t->name;

  if (lower_case_table_names == 2) {
    t->mdl_schema = casedn(cs, t->schema);
    t->mdl_name = casedn(cs, t->name);
  }

  return false;
}

/** Check that the named files of a table are exactly where the table keeps
its tablespaces, and find its DATA DIRECTORY.
@param[in,out]  t   the target
@return true on error, reported with my_error() */
bool check_ibd_location(Ibd_target *t) {
  char schema_fs[FN_REFLEN + 1];
  char table_fs[FN_REFLEN + 1];

  tablename_to_filename(t->schema.c_str(), schema_fs, sizeof(schema_fs));
  tablename_to_filename(t->name.c_str(), table_fs, sizeof(table_fs));

  const std::string dir = real_path(dir_of(t->sdi_file));
  const std::string parent = dir_of(dir);
  const std::string datadir = real_path(mysql_real_data_home);

  if (dir.empty() || !same_file_name(base_of(dir), schema_fs)) {
    return wrong_ibd_file(t->sdi_file,
                          "is not in the directory of the table's schema");
  }

  if (!same_file_name(parent, datadir)) {
    /* A table created with DATA DIRECTORY keeps its schema directory
    there. InnoDB checks that the location is a known directory. */
    t->data_directory = parent;
  }

  const bool partitioned =
      !static_cast<const dd::Table &>(*t->source).leaf_partitions().empty();

  for (const std::string &file : t->files) {
    if (!same_file_name(real_path(dir_of(file)), dir)) {
      return wrong_ibd_file(file,
                            "is not in the same directory as the "
                            "other files of its table");
    }

    const std::string base = base_of(file);

    if (!partitioned) {
      if (!same_file_name(base, std::string(table_fs) + ".ibd")) {
        return wrong_ibd_file(file, "is not named after its table");
      }
    } else {
      const std::string prefix = std::string(table_fs) + "#p#";
      if (base.size() <= prefix.size() ||
          my_strnncoll(
              files_charset_info, reinterpret_cast<const uchar *>(base.c_str()),
              prefix.size(), reinterpret_cast<const uchar *>(prefix.c_str()),
              prefix.size()) != 0) {
        return wrong_ibd_file(file,
                              "is not named after a partition of its "
                              "table");
      }
    }
  }

  return false;
}

/** Check that the user may create and alter the table.
@param[in]  thd   thread handle
@param[in]  t     the target
@return true if not, reported with my_error() */
bool check_ibd_privileges(THD *thd, const Ibd_target &t) {
  for (Access_bitmask want :
       {Access_bitmask(CREATE_ACL), Access_bitmask(ALTER_ACL)}) {
    Table_ref tl(t.mdl_schema.c_str(), t.mdl_schema.length(),
                 t.mdl_name.c_str(), t.mdl_name.length(), t.name.c_str(),
                 TL_IGNORE);

    if (check_access(thd, want, tl.db, &tl.grant.privilege,
                     &tl.grant.m_internal, false, false) ||
        check_grant(thd, want, &tl, false, 1, false)) {
      return true;
    }
  }
  return false;
}

/** Drop what the server that wrote the SDI recorded for its storage engine:
ids, root pages, tablespaces and INSTANT metadata. The new table gets its
own; the engine reads the old ones from the source definition.
@param[in,out]  part   partition and its subpartitions */
void reset_se_private(dd::Partition *part) {
  part->set_se_private_id(dd::INVALID_OBJECT_ID);
  part->se_private_data().clear();
  part->set_tablespace_id(dd::INVALID_OBJECT_ID);

  for (dd::Partition_index *index : *part->indexes()) {
    index->se_private_data().clear();
    index->set_tablespace_id(dd::INVALID_OBJECT_ID);
  }

  for (dd::Partition *sub : *part->subpartitions()) {
    reset_se_private(sub);
  }
}

/** @copydoc reset_se_private(dd::Partition*) */
void reset_se_private(dd::Table *table) {
  table->set_se_private_id(dd::INVALID_OBJECT_ID);
  table->se_private_data().clear();
  table->set_tablespace_id(dd::INVALID_OBJECT_ID);

  for (dd::Column *column : *table->columns()) {
    /* A column dropped with ALGORITHM=INSTANT stays as a hidden column. Its
    INSTANT versions are matched against the tablespace when it is imported,
    so keep them. */
    dd::Properties &p = column->se_private_data();

    if (column->is_se_hidden() && p.exists("version_dropped")) {
      std::unique_ptr<dd::Properties> keep(
          dd::Properties::parse_properties(""));

      for (const char *key :
           {"version_added", "version_dropped", "physical_pos"}) {
        dd::String_type value;
        if (p.exists(key) && !p.get(key, &value)) {
          keep->set(key, value);
        }
      }

      p.clear();
      p.insert_values(*keep);
    } else {
      p.clear();
    }
  }

  for (dd::Index *index : *table->indexes()) {
    index->se_private_data().clear();
    index->set_tablespace_id(dd::INVALID_OBJECT_ID);
  }

  for (dd::Partition *part : *table->partitions()) {
    reset_se_private(part);
  }
}

/** @return true if a table is one of the targets of the statement
@param[in]  targets  the targets
@param[in]  schema   schema name
@param[in]  name     table name */
bool is_ibd_target(const std::vector<Ibd_target> &targets,
                   const dd::String_type &schema, const dd::String_type &name) {
  for (const Ibd_target &t : targets) {
    if (my_strcasecmp(table_alias_charset, t.schema.c_str(), schema.c_str()) ==
            0 &&
        my_strcasecmp(table_alias_charset, t.name.c_str(), name.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

/** Check a target before any table of the statement is created, since a
failure found while creating a later table deletes the files of the tables
already created: the schema exists, the table does not, the spatial
reference systems of its columns exist, and with foreign_key_checks on, every
parent table that is not imported by the same statement exists. The table
locks are held.
@param[in]  thd      thread handle
@param[in]  t        the target
@param[in]  targets  all targets of the statement
@return true on error, reported with my_error() */
bool check_ibd_target(THD *thd, const Ibd_target &t,
                      const std::vector<Ibd_target> &targets) {
  dd::cache::Dictionary_client &dc = *thd->dd_client();
  const dd::Schema *schema = nullptr;

  if (dc.acquire(t.schema, &schema)) {
    return true;
  }

  if (schema == nullptr) {
    my_error(ER_IMP_SCHEMA_DOES_NOT_EXIST, MYF(0), t.schema.c_str());
    return true;
  }

  const dd::Table *existing = nullptr;
  if (dc.acquire(t.schema, t.name, &existing)) {
    return true;
  }

  if (existing != nullptr) {
    my_error(ER_IMP_TABLE_ALREADY_EXISTS, MYF(0), t.schema.c_str(),
             t.name.c_str());
    return true;
  }

  /* As for CREATE TABLE, the reference system of a column must exist. */
  for (const dd::Column *column :
       static_cast<const dd::Table &>(*t.source).columns()) {
    const std::optional<gis::srid_t> srid = column->srs_id();

    if (!srid.has_value() || srid.value() == 0) {
      continue;
    }

    bool exists = false;
    if (Srs_fetcher::srs_exists(thd, srid.value(), &exists)) {
      return true;
    }

    if (!exists) {
      my_error(ER_SRS_NOT_FOUND, MYF(0), srid.value());
      return true;
    }
  }

  if (thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS) {
    return false;
  }

  const dd::Table &source = *t.source;

  for (const dd::Foreign_key *fk : source.foreign_keys()) {
    /* A parent imported by the same statement is checked when the child is
    created, in the order the tables are named. */
    if (is_ibd_target(targets, fk->referenced_table_schema_name(),
                      fk->referenced_table_name())) {
      continue;
    }

    const dd::Table *parent = nullptr;

    if (dc.acquire(fk->referenced_table_schema_name(),
                   fk->referenced_table_name(), &parent)) {
      return true;
    }

    if (parent == nullptr) {
      my_error(ER_FK_CANNOT_OPEN_PARENT, MYF(0),
               fk->referenced_table_name().c_str());
      return true;
    }
  }

  return false;
}

/** Create the table of a target on top of its tablespace files.
@param[in]  thd   thread handle
@param[in]  hton  InnoDB
@param[in]  t     the target
@return true on error, reported with my_error() */
bool create_ibd_target(THD *thd, handlerton *hton, Ibd_target &t) {
  dd::cache::Dictionary_client &dc = *thd->dd_client();
  const dd::Schema *schema = nullptr;

  if (dc.acquire(t.schema, &schema)) {
    return true;
  }

  if (schema == nullptr) {
    my_error(ER_IMP_SCHEMA_DOES_NOT_EXIST, MYF(0), t.schema.c_str());
    return true;
  }

  const dd::Table *existing = nullptr;
  if (dc.acquire(t.schema, t.name, &existing)) {
    return true;
  }

  if (existing != nullptr) {
    my_error(ER_IMP_TABLE_ALREADY_EXISTS, MYF(0), t.schema.c_str(),
             t.name.c_str());
    return true;
  }

  t.source->set_schema_id(schema->id());

  /* With foreign_key_checks on, as for CREATE TABLE, every parent table
  must exist. */
  if (!(thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS)) {
    const dd::Table &source = *t.source;

    for (const dd::Foreign_key *fk : source.foreign_keys()) {
      const dd::Table *parent = nullptr;

      if (my_strcasecmp(table_alias_charset,
                        fk->referenced_table_schema_name().c_str(),
                        t.schema.c_str()) == 0 &&
          my_strcasecmp(table_alias_charset,
                        fk->referenced_table_name().c_str(),
                        t.name.c_str()) == 0) {
        continue;
      }

      if (dc.acquire(fk->referenced_table_schema_name(),
                     fk->referenced_table_name(), &parent)) {
        return true;
      }

      if (parent == nullptr) {
        my_error(ER_FK_CANNOT_OPEN_PARENT, MYF(0),
                 fk->referenced_table_name().c_str());
        return true;
      }
    }
  }

  std::unique_ptr<dd::Table> target(t.source->clone());
  reset_se_private(target.get());

  if (dc.store(target.get())) {
    return true;
  }

  dd::Table *table_def = nullptr;
  if (dc.acquire_for_modification(t.schema, t.name, &table_def) ||
      table_def == nullptr) {
    return true;
  }

  HA_CREATE_INFO create_info;
  create_info.db_type = hton;
  create_info.m_import_source = t.source.get();
  create_info.m_clone_stage_dir = t.clone_stage_dir;

  uint32 key_block_size = 0;
  if (table_def->options().exists("key_block_size") &&
      !table_def->options().get("key_block_size", &key_block_size) &&
      key_block_size != 0) {
    create_info.key_block_size = key_block_size;
    create_info.used_fields |= HA_CREATE_USED_KEY_BLOCK_SIZE;
  }

  if (!t.data_directory.empty()) {
    /* As CREATE TABLE passes it: the directory followed by the table name,
    which the engine replaces with its own file name. */
    const std::string data_file_name =
        t.data_directory + FN_LIBCHAR + t.name.c_str();
    create_info.data_file_name = thd->mem_strdup(data_file_name.c_str());
    create_info.used_fields |= HA_CREATE_USED_DATADIR;
  }

  char path[FN_REFLEN + 1];
  build_table_filename(path, sizeof(path) - 1, t.schema.c_str(), t.name.c_str(),
                       "", 0);

  if (ha_create_table(thd, path, t.schema.c_str(), t.name.c_str(), &create_info,
                      nullptr, true, false, table_def)) {
    return true;
  }

  const dd::Table *new_table = nullptr;
  if (dc.acquire(t.schema, t.name, &new_table) || new_table == nullptr) {
    return true;
  }

  if (adjust_fk_children_after_parent_def_change(
          thd, t.schema.c_str(), t.name.c_str(), hton, new_table, nullptr) ||
      adjust_fk_parents(thd, t.schema.c_str(), t.name.c_str(), true, nullptr)) {
    return true;
  }

  Table_ref tl(t.mdl_schema.c_str(), t.mdl_schema.length(), t.mdl_name.c_str(),
               t.mdl_name.length(), t.name.c_str(), TL_IGNORE);
  Uncommitted_tables_guard uncommitted_tables(thd);
  uncommitted_tables.add_table(&tl);

  return update_referencing_views_metadata(thd, &tl, false,
                                           &uncommitted_tables);
}

}  // namespace

bool Sql_cmd_import_table::execute_ibd(THD *thd) {
  std::vector<std::string> files;

  for (auto &pattern : m_sdi_patterns) {
    LEX_STRING converted = pattern;

    if (thd->charset() != files_charset_info &&
        thd->convert_string(&converted, files_charset_info, pattern.str,
                            pattern.length, thd->charset())) {
      return true;
    }

    dd::sdi_file::Paths_type paths{key_memory_DD_import};
    if (dd::sdi_file::expand_ibd_pattern(thd, converted, &paths)) {
      return true;
    }
    for (auto &p : paths) {
      files.emplace_back(p.first.c_str(), p.first.length());
    }
  }

  if (import_ibd_files(thd, files, nullptr)) {
    return true;
  }

  my_ok(thd);
  return false;
}

bool import_ibd_files(THD *thd, const std::vector<std::string> &files,
                      const char *clone_stage_dir) {
  handlerton *hton = ha_resolve_by_legacy_type(thd, DB_TYPE_INNODB);

  if (hton == nullptr || hton->sdi_get_from_file == nullptr) {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "IMPORT TABLE from tablespace files without InnoDB");
    return true;
  }

  /* The engine finishes the DDL after commit or rollback; after rollback it
  deletes the tablespace files it already converted. Declared first, so it
  runs after the rollback guard. */
  auto post_ddl_guard = dd::sdi_utils::make_guard(thd, [&](THD *) {
    if (hton->post_ddl != nullptr) {
      hton->post_ddl(thd);
    }
  });

  auto rbgrd = dd::sdi_utils::make_guard(thd, [&](THD *) {
    trans_rollback_stmt(thd);
    trans_rollback(thd);
  });

  // Need to keep this alive until after commit/rollback has been done
  const dd::cache::Dictionary_client::Auto_releaser ar{thd->dd_client()};

  if (check_readonly(thd, true)) {
    return true;
  }

  /* Read the table definitions from the files. Only the tablespace file of
  a table, or the first partition of a partitioned table, holds one. */
  std::vector<Ibd_target> targets;
  std::vector<std::string> partition_files;
  std::vector<std::string> seen;

  for (const std::string &file : files) {
    const std::string real = real_path(file);

    if (std::find(seen.begin(), seen.end(), real) != seen.end()) {
      continue;
    }
    seen.push_back(real);

    if (check_ibd_file_is_free(thd, file)) {
      return true;
    }

    Sdi_from_file sdi;
    if (hton->sdi_get_from_file(thd, file.c_str(), &sdi)) {
      return true;
    }

    if (sdi.shared_tablespace || sdi.tables.size() > 1) {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "IMPORT TABLE from a general tablespace");
      return true;
    }

    if (sdi.tables.empty()) {
      partition_files.push_back(file);
      continue;
    }

    Ibd_target t;
    t.sdi_file = file;
    t.clone_stage_dir = clone_stage_dir;
    if (load_ibd_target(thd, sdi.tables.front(), &t)) {
      return true;
    }
    targets.push_back(std::move(t));
  }

  /* Give every partition file to its table. A partitioned table must be
  named with all its partitions and nothing else. */
  std::vector<bool> used(partition_files.size(), false);

  for (Ibd_target &t : targets) {
    t.files.push_back(t.sdi_file);

    const size_t n_parts =
        static_cast<const dd::Table &>(*t.source).leaf_partitions().size();
    if (n_parts == 0) {
      continue;
    }

    const std::string sdi_base = base_of(t.sdi_file);
    const size_t sep = sdi_base.find("#p#") != std::string::npos
                           ? sdi_base.find("#p#")
                           : sdi_base.find("#P#");
    if (sep == std::string::npos) {
      return wrong_ibd_file(t.sdi_file,
                            "holds a partitioned table but is "
                            "not a partition file");
    }

    const std::string prefix = sdi_base.substr(0, sep + 3);
    const std::string dir = real_path(dir_of(t.sdi_file));

    for (size_t i = 0; i < partition_files.size(); ++i) {
      const std::string base = base_of(partition_files[i]);

      if (!used[i] && real_path(dir_of(partition_files[i])) == dir &&
          base.size() > prefix.size() &&
          my_strnncoll(
              files_charset_info, reinterpret_cast<const uchar *>(base.c_str()),
              prefix.size(), reinterpret_cast<const uchar *>(prefix.c_str()),
              prefix.size()) == 0) {
        used[i] = true;
        t.files.push_back(partition_files[i]);
      }
    }
  }

  for (size_t i = 0; i < partition_files.size(); ++i) {
    if (used[i]) {
      continue;
    }

    /* Named after a partition of a table being imported, but elsewhere. */
    const std::string base = base_of(partition_files[i]);
    for (const Ibd_target &t : targets) {
      const std::string sdi_base = base_of(t.sdi_file);
      const size_t sep = sdi_base.find("#p#") != std::string::npos
                             ? sdi_base.find("#p#")
                             : sdi_base.find("#P#");
      if (sep != std::string::npos && base.size() > sep + 3 &&
          my_strnncoll(files_charset_info,
                       reinterpret_cast<const uchar *>(base.c_str()), sep + 3,
                       reinterpret_cast<const uchar *>(sdi_base.c_str()),
                       sep + 3) == 0) {
        return wrong_ibd_file(partition_files[i],
                              "is not in the same directory as the "
                              "other files of its table");
      }
    }

    return wrong_ibd_file(partition_files[i],
                          "holds no table definition and is not a "
                          "partition of a table being imported");
  }

  for (const Ibd_target &t : targets) {
    const size_t n_parts =
        static_cast<const dd::Table &>(*t.source).leaf_partitions().size();

    if (n_parts != 0 && t.files.size() != n_parts) {
      std::string msg("IMPORT TABLE of `");
      msg.append(t.schema.c_str()).append("`.`").append(t.name.c_str());
      msg.append("` needs all ").append(std::to_string(n_parts));
      msg.append(" partition files, ").append(std::to_string(t.files.size()));
      msg.append(" were named");
      my_error(ER_TABLE_SCHEMA_MISMATCH, MYF(0), msg.c_str());
      return true;
    }
  }

  for (size_t i = 0; i < targets.size(); ++i) {
    for (size_t j = i + 1; j < targets.size(); ++j) {
      if (targets[i].mdl_schema == targets[j].mdl_schema &&
          targets[i].mdl_name == targets[j].mdl_name) {
        return wrong_ibd_file(targets[j].sdi_file,
                              "defines a table that another named file "
                              "defines too");
      }
    }
  }

  MDL_request_list mdl_requests;
  std::vector<dd::String_type> schema_names;

  for (Ibd_target &t : targets) {
    if (check_ibd_location(&t) || check_ibd_privileges(thd, t)) {
      return true;
    }

    /* Before any file is changed: the named files must be exactly the
    table's files. */
    if (hton->sdi_check_import_files != nullptr &&
        hton->sdi_check_import_files(thd, t.schema.c_str(), *t.source,
                                     t.files)) {
      return true;
    }

    MDL_request *req = new (thd->mem_root) MDL_request;
    MDL_REQUEST_INIT(req, MDL_key::TABLE, t.mdl_schema.c_str(),
                     t.mdl_name.c_str(), MDL_EXCLUSIVE, MDL_TRANSACTION);
    mdl_requests.push_front(req);
    schema_names.push_back(t.mdl_schema);

    /* As for CREATE TABLE: the parents of the table's foreign keys and the
    tables already referencing it get their definitions updated. */
    const dd::Table &source = *t.source;

    for (const dd::Foreign_key *fk : source.foreign_keys()) {
      char buff_db[NAME_LEN + 1];
      char buff_table[NAME_LEN + 1];
      my_stpncpy(buff_db, fk->referenced_table_schema_name().c_str(), NAME_LEN);
      my_stpncpy(buff_table, fk->referenced_table_name().c_str(), NAME_LEN);

      if (lower_case_table_names == 2) {
        my_casedn_str(system_charset_info, buff_db);
        my_casedn_str(system_charset_info, buff_table);
      }

      MDL_request *parent = new (thd->mem_root) MDL_request;
      MDL_REQUEST_INIT(parent, MDL_key::TABLE, thd->mem_strdup(buff_db),
                       thd->mem_strdup(buff_table), MDL_EXCLUSIVE,
                       MDL_STATEMENT);
      mdl_requests.push_front(parent);
    }

    if (collect_fk_children(thd, t.mdl_schema.c_str(), t.mdl_name.c_str(), hton,
                            MDL_EXCLUSIVE, &mdl_requests)) {
      return true;
    }
  }

  std::sort(schema_names.begin(), schema_names.end());
  schema_names.erase(std::unique(schema_names.begin(), schema_names.end()),
                     schema_names.end());

  for (auto &sn : schema_names) {
    MDL_request *r = new (thd->mem_root) MDL_request;
    MDL_REQUEST_INIT(r, MDL_key::SCHEMA, sn.c_str(), "",
                     MDL_INTENTION_EXCLUSIVE, MDL_TRANSACTION);
    mdl_requests.push_front(r);
  }

  MDL_request *backup_lock = new (thd->mem_root) MDL_request;
  MDL_REQUEST_INIT(backup_lock, MDL_key::BACKUP_LOCK, "", "",
                   MDL_INTENTION_EXCLUSIVE, MDL_TRANSACTION);
  mdl_requests.push_front(backup_lock);

  // If we cannot acquire protection against GRL, err out.
  if (thd->global_read_lock.can_acquire_protection()) return true;

  MDL_request *grl = new (thd->mem_root) MDL_request;
  MDL_REQUEST_INIT(grl, MDL_key::GLOBAL, "", "", MDL_INTENTION_EXCLUSIVE,
                   MDL_TRANSACTION);
  mdl_requests.push_front(grl);

  /* Like ALTER TABLE ... IMPORT TABLESPACE, wait for LOCK TABLES FOR
  BACKUP. */
  if (thd->backup_tables_lock.abort_if_acquired()) return true;

  MDL_request *backup_tables = new (thd->mem_root) MDL_request;
  thd->backup_tables_lock.init_protection_request(backup_tables,
                                                  MDL_TRANSACTION);
  mdl_requests.push_front(backup_tables);

  if (thd->mdl_context.acquire_locks(&mdl_requests,
                                     thd->variables.lock_wait_timeout)) {
    return true;
  }

  // Now when we have protection against concurrent change of read_only
  // option we can safely re-check its value.
  if (check_readonly(thd, true)) return true;

  for (auto &sn : schema_names) {
    if (check_schema_readonly(thd, sn.c_str())) return true;
  }

  for (const Ibd_target &t : targets) {
    if (check_ibd_target(thd, t, targets)) {
      return true;
    }
  }

  for (Ibd_target &t : targets) {
    if (create_ibd_target(thd, hton, t)) {
      return true;
    }
  }

  if (trans_commit_stmt(thd) || trans_commit(thd)) {
    return true;
  }

  rbgrd.release();
  return false;
}
