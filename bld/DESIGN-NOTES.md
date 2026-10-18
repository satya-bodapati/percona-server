# Partial clone: clone a table or a database from a remote donor

Status: **design = section 28 (lockless: clone + undo driver + import), 2026-10-18.** Readiness in section 29. Decisions are logged in "Decisions" as we agree on them.

## 1. How clone works today (PS 8.4)

### SQL and plugin layer (plugin/clone, sql/sql_admin.cc, sql/clone_handler.cc)
- Syntax: `CLONE LOCAL DATA DIRECTORY 'd'` and
  `CLONE INSTANCE FROM u@h:p IDENTIFIED BY 'pw' [DATA DIRECTORY 'd'] [REQUIRE [NO] SSL]`.
  The `Sql_cmd_clone` class lives in sql_admin.cc.
- Privileges: replacing the datadir needs CLONE_ADMIN. LOCAL and DATA DIRECTORY need BACKUP_ADMIN on the
  recipient. The donor needs BACKUP_ADMIN. `clone_valid_donor_list` must name the donor.
- Wire: `COM_CLONE` turns a normal client connection into a clone session. The donor's `Server::clone()`
  runs the command loop: COM_INIT / ATTACH / REINIT / EXECUTE / ACK / EXIT.
  - Replies are COM_RES_LOCS, DATA_DESC, DATA, PLUGIN_V2, COLLATION, CONFIG(_V3), COMPLETE and ERROR.
  - Protocol 0x0102 in 8.4. Trunk uses 0x0103 for JSON version configs (LTS to next LTS), so our bump must
    come after it.
- COM_INIT carries the version, ddl_timeout (bit 31 = no backup lock) and one opaque locator per SE.
  - There is no field to scope the snapshot.
  - The `Mysql_clone` plugin interface (local/client/server) has no object-list argument either.
- The donor sends all plugins, charsets and configs (version, OS, page size, ...). The recipient checks they
  match (`Client::validate_remote_params`). For one table that check is too broad.
- Tasks: the master session plus worker connections (COM_ATTACH), auto-tuned up to `clone_max_concurrency`.
  - After a network error the master reconnects with COM_REINIT. The locator carries the chunk bitmap, so
    the copy resumes where it stopped.

### InnoDB (storage/innobase/clone)
- One `Clone_Snapshot` is shared by the clone handles. Each `Clone_Handle` (COPY or APPLY) owns a
  `Clone_Task_Manager`.
  - A chunk is 4096 pages and a block is 64 pages.
  - Descriptors: Locator, Task_Meta, State, File_MetaData, Data.
- States, in `Clone_Snapshot::init_*_copy` (clone0copy.cc):
  - **INIT**: `clone_init_tablespaces` opens every DD tablespace.
  - **FILE_COPY**: starts page tracking (pages are recorded when flushed). Then raw OS reads of every
    `fil_space`: ibdata, undo, mysql.ibd, all user .ibd files and general tablespaces. The only filter is
    `FIL_TYPE_TABLESPACE` and not deleted.
  - **PAGE_COPY**: starts redo archiving (from a fresh checkpoint), then stops page tracking. Sends the
    tracked pages from the buffer pool. Page 0 of every encrypted space is re-sent with the key in plain
    text.
  - **REDO_COPY**: blocks XA, waits for binlog-prepared transactions, flushes GTIDs and writes the binlog
    position to the trx sys page. Then stops redo archiving: **that stop LSN is the snapshot point**.
    Ships the archived redo.
- Consistency comes from crash recovery of the whole instance on the recipient.
  - Fuzzy pages plus redo from the archive checkpoint to the end LSN (`recv_sys->is_cloned_db`).
  - Then rollback of uncommitted transactions from the donor's undo, then purge.
- DDL during the clone:
  - `clone_block_ddl` (default OFF) holds the backup lock for the whole clone. Otherwise the clone is told
    about each DDL through `Clone_notify`, which uses the snapshot state to decide what to do.
  - Drop and rename wait for the file to be unpinned. Bulk/encryption ALTER waits for PAGE_COPY to end.
  - IMPORT TABLESPACE and disabling redo fail with ER_CLONE_IN_PROGRESS.
  - New or changed files are picked up again at each state change.
- Encryption:
  - The keys travel in clear, so SSL is forced (`set_secure`). The recipient re-wraps them with its own
    master key (`encrypt_key_in_header`, `encrypt_key_in_log_header`).
  - Cloned-db recovery skips redo key records. The donor keyring is never sent.
- Recipient, replacing the datadir:
  - `clone_drop_user_data` drops every user table, schema and tablespace through SQL.
    `clone_drop_binary_logs` then runs RESET BINARY LOGS AND GTIDS.
  - Files are staged as `*.#clone` plus `#clone/#replace_files`, and the server restarts.
  - At startup `clone_files_recovery` rolls the files forward, then redo recovery runs.
  - `fix_cloned_tables` creates empty MyISAM/CSV tables, and the GTIDs are written.
- Recipient, DATA DIRECTORY: the directory must not exist. No drop, no restart. Someone starts mysqld on it
  later.
- The DD is the donor's mysql.ibd, copied as pages. Nothing is rebuilt from SDI.

### Trunk (9.x) differences
- Protocol V4 (JSON version config, LTS to next LTS).
- Hardening checks on file and task indexes.
- Descriptor format moved to `include/clone_descriptor_format.h`.
- Zero-fill file extension.
- No change to the state machine, the DDL handling or the recovery model.

## 2. Why one table is hard

1. **Consistency.** Pages are fuzzy and only the instance-wide redo makes them consistent. The undo that
   rolls back open transactions is instance-wide too.
   - A single .ibd needs one of: a donor-side quiesce (no writer on the table at the copy point), or
     space-filtered redo apply plus a way to remove uncommitted rows without donor undo.
   - Its `DB_TRX_ID` and `DB_ROLL_PTR` point into donor rollback segments, so they must be reset the way
     IMPORT does (`PageConverter::adjust_cluster_record`, delete-marked purge).
2. **LSN space.** Donor page LSNs are foreign to a running recipient.
   - IMPORT already rewrites the page LSN and space_id (`PageConverter`).
   - Applying redo cannot happen inside the recipient's live recovery. It needs a separate apply step,
     as in PXB prepare.
3. **DD/SDI.** The recipient keeps its own DD, so the table must be created there from the donor's
   definition: from the SDI in the .ibd, or from `SHOW CREATE` plus a .cfg.
   - Instant DDL row versions need the exact column history (.cfg v7 / SDI). PXB-3751 broke on this.
   - Autoinc (PXB-2264) and a new space_id on the recipient also have to be handled.
4. **Objects with more than one file or link:**
   - **Partitions**: one tablespace each. All or nothing? Import doesn't check partition definitions.
   - **FTS**: auxiliary tables and the config table. Import does not support FTS; rebuilding it on the
     recipient is the simple option. Our vector aux tables have the same shape.
   - **General tablespaces**: hold many tables. Refuse them, or copy the whole tablespace?
   - **Foreign keys**: are the parents required? Import does not check FKs.
5. **Encryption.** Per-table keys go through the existing plain-key-over-SSL path plus a re-wrap (or a .cfp
   equivalent). Undo and redo encryption only matter if we ship redo.
6. **Donor locking and DDL.** We need MDL on the selected tables for the copy (or `clone_block_ddl`). How
   long are writers blocked: the whole copy, or only the final consistent point?
7. **Recipient state.**
   - The table must not exist yet, or there must be a REPLACE option.
   - Replication: IMPORT is binlogged as DDL, so the recipient's replicas would get the table definition
     and no data.
   - GTID/binlog fixups do not apply.
   - Only one clone at a time (`Clone_handler`). Do we allow a partial clone next to a running donor role?
8. **Checks.** Narrow the instance-wide plugin/config match to what one table needs: page size, row
   format, collations, version series, keyring.

## 3. Prior art
- **Nobody asked for "partial clone" by name**: none in PS/PXB/PXC/K8SPS Jira, MySQL bugs or worklogs. The
  closest is **PXB-2745** "Single Table Restore Redesign" (On Hold). It wants to restore chosen tables into
  a running server. PXB-3153 (On Hold) would generate CREATE TABLE from the SDI at `--export`.
- **PXB today**: `--tables/--databases` filter only the .ibd files. ibdata, undo and mysql.ibd are always
  copied, because prepare is a full crash recovery.
  - `--prepare --export` runs a slow shutdown, rebuilds the dict from SDI, and writes .cfg/.cfp
    (`xb_export_cfg_write`, `xb_export_cfp_write`).
  - The user then runs CREATE, DISCARD, copies the files and runs IMPORT.
  - Known breakages: FTS, instant row versions (PXB-3751), autoinc (PXB-2264), partitions, and a stale DD
    after copy-back of a partial backup (PXB-2178).
- **MySQL docs**: clone limits are same series and the next LTS, InnoDB only, one clone at a time.
  Transportable tablespaces are file-per-table only; FTS is unsupported; the .cfg is required for instant
  columns; FK and partition definitions are not checked.
- **Elsewhere**:
  - MySQL Shell `util.copyTables/copySchemas` is online but logical.
  - Vitess MoveTables is logical.
  - TiDB BR and PolarDB table restore copy physical data per table.
  - Aurora clones only the whole cluster.

## 4. Candidate designs

### A. Transportable tablespace over the clone wire, into a running recipient
- **Syntax**: `CLONE TABLE db.t[, ...] | DATABASE db FROM u@h:p IDENTIFIED BY ... [AS db2.t2]`.
- **Donor**:
  - takes MDL and a table S lock on the selected tables, like FLUSH TABLES ... FOR EXPORT (`row_quiesce_*`:
    stop purge, merge the change buffer, flush);
  - streams the SDI, a .cfg-equivalent and the key (over SSL), then the .ibd files, with the existing
    chunk/task/restart model and no redo.
- **Recipient**: creates the table from the SDI, discards it, writes the file, then runs the existing
  IMPORT code (space_id/LSN rewrite, trx_id reset, instant metadata).
- **For**: no redo or undo problem; reuses import; no restart.
- **Against**: writers on the table are blocked for the whole copy; FTS and partition limits come from
  import.

### B. Online fuzzy copy plus space-filtered redo, a short quiesce at the end, apply on the recipient
- FILE_COPY and PAGE_COPY are filtered to the selected space_ids, and the redo archive is filtered by
  space_id.
- At the end point the donor waits briefly until no open transaction has modified the table (a table S lock
  or IX drain). The stop LSN then holds no uncommitted change to it.
- The recipient applies the filtered redo to the staged .ibd in a separate apply step (a recovery engine
  that works on files outside the live instance), then imports as in A.
- **For**: writers are blocked only for a moment, which is real clone semantics.
- **Against**: the separate redo apply is the big new piece of code. Purge and the change buffer interact
  with filtered redo.

### C. New instance with the user tablespaces filtered (DATA DIRECTORY or replace)
- This is PXB partial backup inside clone. Ship everything except the user .ibd files that were not
  selected.
- At first startup, drop the DD entries of the missing tables (fix_cloned_tables already has a fixup pass).
- **For**: very small change to the snapshot (an `add_node` filter plus redo apply that tolerates missing
  spaces); full consistency for free.
- **Against**: not "into a running server"; still copies undo, ibdata and mysql.ibd (possibly large).
  Grants and users come with them. It is useful mainly to provision a test or reporting instance with one
  schema.

### D. Logical (baseline)
- Stream rows from a consistent read view, as MySQL Shell copyTables does.
- Listed only to compare against. It is what a user can do today without new server code.

The order for discussion that fits the code best: A first (it reuses clone transport and import, both
proven), C as a cheap separate mode, and B as the online follow-up once A's recipient side exists.

## 5. Open questions for Satya
1. What is the target use case: add a table into a running server (A/B), or provision a new instance with
   a subset (C)? This decides almost everything else.
2. Is blocking writers on the copied tables for the whole copy acceptable for a first version (A)?
3. Should the syntax extend `CLONE INSTANCE ... TABLES/DATABASES (...)` or add `CLONE TABLE` / `CLONE
   DATABASE`? Should renaming the target (`AS`) be allowed?
4. What if the table already exists on the recipient: error or REPLACE?
5. What scope for partitions, FTS (refuse, or rebuild after import), general tablespaces (refuse?), FK
   parents (require them or ignore), and vector aux tables?
6. Privileges: on the donor, BACKUP_ADMIN plus SELECT on the tables? On the recipient, CREATE/ALTER/DROP on
   the target schema instead of CLONE_ADMIN?
7. Replication on the recipient: binlog it as DDL only (replicas get no data), or refuse when binlogging is
   on?
8. Is this Percona-only syntax and protocol (it bumps the protocol past trunk's 0x0103), or something we try
   to upstream?

## Decisions
- 2026-10-17 (Satya): design B, done as a **filtered clone into a staging area**. ibdata, mysql.ibd, undo
  and redo are shipped and thrown away afterwards. This is a filtered backup inside clone. Next to settle:
  how the staging recovery runs, how the files get into the running server, and the syntax.
- 2026-10-17: integration = `IMPORT TABLE FROM '<file>.ibd'` from the single-table-restore work
  (ps-8.4-import-refactor); partial clone builds on that branch.
- 2026-10-17 (Satya): no second mysqld. Redo and undo are applied inside the running recipient's InnoDB.
  R1/R2 are dropped; the plan is section 8.
- 2026-10-17 (Satya): the redo applier is an independent class (upstream WL#15560 in 26.7.0), so the
  hard part is undo. See section 9.
- 2026-10-17 (Satya): the feature moves to the 26.07 line, where WL#15560 exists.
  - PS `origin/trunk` and `origin/release-26.7.0-1` both contain f615971822e3. 8.4 is no longer the base.
  - The import attach path (ps-8.4-import-refactor) has to be ported to that line too.
- 2026-10-17 (Satya): U2 is too advanced, and clone is one-directional (donor → recipient). The
  recipient → donor fix-up round trip is out. Undo must work from what the donor sends.
- 2026-10-17 (Satya): no redo and no undo shipping. Use **page tracking and send the changed pages**, with
  a short write lock at the end; short is acceptable. Plan in section 10. It needs no redo applier, so the
  move to 26.07 has to be confirmed again (see section 10).
- 2026-10-17 (Satya): the online, short-lock export is a general problem, so **export** should solve it
  for everybody, not clone alone. Clone becomes a transport on top of it. Section 11.
- 2026-10-17 (Satya): **clone is parked.** Focus now: make FLUSH TABLES ... FOR EXPORT online, writing a
  delta file (or other files) that also makes import easier. Section 12.
- 2026-10-17 (Satya): a small lock at the end is OK, as a working assumption.
- 2026-10-17 (Satya): UNLOCK creating a .delta file is not clean. Alternatives in section 13.
- 2026-10-17 (Satya): no UNLOCK at all. The export is one statement that takes and releases its short
  lock itself. This fits E1 (`EXPORT TABLE ... INTO`); E2 is out.
- 2026-10-17 (Satya): INTO is the better form, but **the target must not be assumed local**. S3 may come
  later. Open: how a reader tells an interrupted transfer from a complete one when the export is streamed.
  Section 14.
- 2026-10-17 (Satya): ideally the export writes the metadata into the **SDI of the .ibd** rather than into
  side files. Section 15.
- 2026-10-17 (Satya): back to clone. The idea is a **small lock, plus sending buffer pool pages**, so the
  recipient can simply import. Section 16.
- 2026-10-17 (Satya): **several rounds of page tracking is bad design; we need a solid design.** Rounds are
  dropped. The proposal is section 17: a copy-on-write snapshot taken under a short lock at the **start**.
- 2026-10-17 (Satya): COW does not exist in InnoDB, so don't invent a new mechanism. **Section 17 is
  withdrawn.** Section 18 uses only existing mechanisms.
- 2026-10-17 (Satya): for 8.4, which has no Redo_applier, create a redo applier class. Section 19.
- 2026-10-17 (Satya): applying redo to one tablespace is easy enough to **write by hand, reusing the
  InnoDB functions**. No port of the upstream class. Sketch in section 20.
- 2026-10-18 (Satya): page-only transfer (section 21) is out: under continuous DML it never converges, so
  the lock time depends on the workload. Redo apply stays.
- 2026-10-18 (Satya): on 8.4, **only a driver** around the existing recovery apply functions (section 20,
  isolation in section 22). No port of upstream's class. On 26.07 the same driver sits on `Redo_applier`.
- 2026-10-18 (Satya): **design locked**, section 23.
- 2026-10-18 (Satya): protocol change agreed: new clone protocol version (value agreed against trunk's
  0x0103) plus the table list in the request; the data stream is unchanged. An older donor answers
  with the lower version and the recipient refuses cleanly.
- 2026-10-18 (Satya): streaming a clone to S3 (multipart upload; MySQL Shell has upload code) is a
  **separate feature, "clone cloud"**, designed in its own session (ps-clone-cloud-design).
- 2026-10-18 (Satya): CLONE DATABASE holds a **DDL lock for the whole clone** (clone's backup lock, as a
  full clone takes by default). No consistency across concurrent DDL. Section 25.
- 2026-10-18 (Satya): the table-lock drain is too fragile for many tables. Section 26's MVCC corrections
  are out: sending keys is the logical way and has never been done for a physical copy. **Physical
  consistency relies on undo**, as PXB and full clone do. Section 27.
- 2026-10-18 (Satya): no table locks at all. Section 28: undo tablespaces travel as in a full clone, the
  recipient rolls back the open trx with an undo driver on the attached-raw table, then import's cleanup.
  Section 23's drain is superseded.
- 2026-10-18 (Satya): **8.4 first**, 26.07 after. Syntax = section 7. CLONE DATABASE clones tables
  only (other objects skipped with a warning). FKs to tables outside the set: **import unchecked**
  (FOREIGN_KEY_CHECKS=0 style). Defaults accepted unless objected: protocol 0x0104, name clash fails,
  v1 refuses COMPRESSED row format / general tablespaces / compressed columns / change buffering on;
  partitions allowed; FULLTEXT allowed with automatic rebuild.

## 6. Design B, staged: the flow (draft)

```
donor                                   recipient (running)
-----                                   -------------------
FILE/PAGE/REDO_COPY, filtered:          #clone_stage/<id>/  (receives the files)
  ibdata, undo, mysql.ibd, redo  ---->    throwaway system files
  + .ibd of the selected tables  ---->    selected .ibd files
                                        (1) staging recovery: redo, rollback, purge
                                        (2) export: .cfg/.cfp + the definition, per table
                                        (3) integrate: create + import into the live DD
                                        (4) remove #clone_stage/<id>
```

### Donor changes
- Small. `Clone_Snapshot::add_node` keeps every non-user space (system, undo, DD) plus the selected
  space_ids.
- `add_page` already drops pages of spaces that are not in `m_data_file_map`.
- The redo is sent whole, as today; the staging recovery skips the spaces it doesn't have (below).
- The object list goes in COM_INIT (protocol bump) and in the InnoDB locator, which turns names into
  space_ids under MDL.
- Writers are never blocked: rollback in the staging area removes uncommitted rows, so B no longer needs
  the end-of-copy quiesce.
- Cost: undo and redo are sized for the whole instance, as in PXB partial backup.

### (1) Staging recovery: where it runs (Satya: "running recovery has to be thought through too")
InnoDB is a process singleton: `log_sys`, `recv_sys`, `fil_system`, `buf_pool`, `trx_sys` and the DD.
A second instance cannot recover inside the running recipient. The options:

- **R1. A child mysqld on the staging dir.** The recipient fork/execs its own binary in a one-shot mode
  (`--clone-prepare`, say, a sibling of `--initialize`):
  - skip-networking, a small buffer pool;
  - full recovery: redo, then rollback, then a slow shutdown for purge and change-buffer merge;
  - writes .cfg/.cfp/.sdi per selected table, then exits.
  - **For**: full fidelity, reuses everything, no new recovery code.
  - **Against**:
    - process management inside the server (memory, a hung child, kill on KILL QUERY or on shutdown);
    - the keyring has to be shared with the child, because the keys are already re-wrapped with the
      recipient's master key. Two processes would use one keyring file or component;
    - an AppArmor/SELinux or container policy may refuse the exec;
    - the child boots the donor's mysql.ibd. A point-release difference means a DD upgrade in the
      staging area (clone already allows that today).
- **R2. The user runs that step** (a two-statement flow, like PXB prepare). Simple, poor UX. It could be a
  fallback for R1.
- **R3. A redo applier inside the recipient for detached files.**
  - Parse the shipped redo and apply the records for the selected spaces to pages read from the staged
    .ibd into private frames (`recv_parse_or_apply_log_rec_body` on a block outside the buffer pool).
  - No undo means no rollback, so it needs the end-of-copy quiesce from the original B, and the
    throwaway files are not needed at all.
  - **For**: no child process, less data shipped.
  - **Against**: a large new piece of code in the most sensitive part of InnoDB.

Satya's "throwaway ibdata/undo/mysql.ibd" choice points to R1 (or R2).

Checked in code so far:
- Recovery already skips redo for spaces it cannot find. `recv_apply_log_rec` puts them in
  `recv_sys->missing_ids`, which is what PXB partial prepare relies on.
- To verify: does rollback of a transaction that touched a missing table skip quietly? Does the DD in the
  staging area complain about the missing .ibd files at boot (PXB-2589 warnings)?
- Should the child mark the unselected spaces as deleted, so they never count as missing?

### (3) Integrate into the running server: reuse IMPORT TABLE FROM '<file>.ibd'
The single-table-restore work (PXB-2745) already built the integration step:
- Branch `ps-8.4-import-refactor` (`~/WORK/ps/ps-8.4-import-refactor`, pushed to fork, no PR).
- Its design: `~/WORK/pxb/pxb-single-table-restore/bld/DESIGN.md` and `INTERFACE.md`.

The branch has four commits:
- 743d45b7e2a splits the InnoDB import into reusable steps;
- acf462f1221 reads the SDI of a file that is not attached;
- a188f7737df lets InnoDB create a table on top of an existing file;
- 3debe50b2f9 adds `IMPORT TABLE FROM 'db/t.ibd'[, ...]`.

What the statement does, and what we inherit:
- **Table definition**: comes from the SDI in the .ibd. No .cfg needed, instant row versions included,
  except INSTANT columns from before 8.0.29 (PS-9459).
- **Atomicity**: the whole statement imports all or none. log_ddl deletes the converted files on failure or
  on a crash.
- **Foreign keys**: linked both ways. A missing parent is allowed with foreign_key_checks=0.
- **AUTO_INCREMENT**: MAX+1 is persisted.
- **Partitions**: every partition file is named, by name or by glob.
- **Encryption**: needs the .cfp next to the .ibd.
- **Privileges and binlog**: needs CREATE and ALTER on each table, not FILE. Not binlogged.
- **Refused**: general tablespaces, compressed columns (v2) and non-InnoDB tables.
- **FULLTEXT**: the index comes back corrupted and must be rebuilt (drop all, then add each back).
- **File location**: the file must already be at the table's own path. The server never moves files and
  takes ownership of the file.

The `xtrabackup --restore` client does the steps the statement doesn't:
- creates missing databases with the original charset;
- sets the AUTO_INCREMENT from the backup's counter;
- rebuilds FULLTEXT;
- runs CREATE TRIGGER with the original definer.

The same DESIGN.md also says:
- **FLUSH ... FOR EXPORT is discarded**: blocking writes is a no-go, which also rules out design A here.
- Prepare on a subset (ibdata, undo, mysql.ibd plus the table files) in a scratch directory works, because
  redo for absent spaces is skipped. That is our staging recovery.

So **partial clone = filtered clone into a stage + staging recovery + IMPORT TABLE FROM '.ibd'**, with
clone doing the `--restore` client steps inside the server.

Per table, after the staging recovery:
1. **Stage location**: put the stage inside the datadir (`#clone_stage/<id>/`), so that moving each .ibd to
   `<datadir>/<db>/<t>.ibd` is a rename, not a copy. A DATA DIRECTORY table needs a copy, or the stage on
   that filesystem.
2. **Encrypted tables**: the staging child writes the .cfp. Clone deletes it after the import, because it
   holds the key in plain text.
3. **Import**: run IMPORT TABLE FROM, one statement per database, or per table.
4. **Metadata**: the staging DD (the donor's mysql.ibd as of the snapshot) gives the schema charset, the
   AUTO_INCREMENT counter and the triggers. The child must emit them, for example as a small manifest per
   table, because the recipient cannot read a second mysql.ibd. This is the same need as the "the .ibd
   should carry everything" future item in DESIGN.md (autoinc in the SDI, SDI type 3 for triggers).
5. **FULLTEXT**: rebuild after import, or import the aux files once v2 lands. Vector aux tables are the same
   question.
6. **Replication**: same as `--restore`: local only, not binlogged. Its v2 ideas (`REPLICATION_IDEAS.md`)
   would also apply here.
7. **Concurrent PXB backup**: the reduced-lock rules in DESIGN.md (flush and checkpoint before commit, a new
   space id) are kept by the IMPORT path itself.

## 7. Syntax (draft for discussion)

```sql
CLONE TABLE    db.t1 [, db.t2 ...] FROM 'user'@'host':port IDENTIFIED BY 'pw'
      [REQUIRE [NO] SSL];
CLONE DATABASE db1 [, db2 ...]     FROM 'user'@'host':port IDENTIFIED BY 'pw'
      [REQUIRE [NO] SSL];
```

- CLONE, TABLE and DATABASE are already keywords, and CLONE LOCAL/INSTANCE stay as they are. No new
  keyword unless we add staging or rename options.
- Later, maybe:
  - `CLONE TABLE db.t AS db2.t2` (rename). IMPORT TABLE takes schema and table from the SDI, so a rename
    needs SDI rewriting. Not in v1;
  - an existing target is an error (as in IMPORT TABLE); `REPLACE` later, if at all;
  - `STAGING DIRECTORY '<path>'`;
  - `CLONE INSTANCE ... DATA DIRECTORY 'd' DATABASES (db1)`, which is design C almost for free: the same
    filter with no import.
- Why not `CLONE INSTANCE ... TABLES (...)`: INSTANCE without DATA DIRECTORY means "wipe and replace", and
  a filter must never inherit that.
- Privileges:
  - recipient: BACKUP_ADMIN plus what IMPORT TABLE and the `--restore` steps need: CREATE and ALTER on
    the tables, CREATE for a new database, TRIGGER, and SET_ANY_DEFINER for triggers owned by someone
    else. Not CLONE_ADMIN.
  - donor: BACKUP_ADMIN plus SELECT on the selected tables?
- The donor list stays `clone_valid_donor_list`.

## 8. In-process staging recovery (draft, follows Satya 2026-10-17)

### What to ship
- The selected .ibd files, the donor's **undo tablespaces**, and the redo archive.
- ibdata and mysql.ibd are no longer needed:
  - the definition comes from the SDI;
  - the donor's SQL layer sends the rest (AUTO_INCREMENT, triggers, schema charset) as a metadata record
    in the stream;
  - in 8.4 the user rollback segments are all in the undo tablespaces;
  - the change buffer is excluded by the rule below.
- **Change buffer**: entries for secondary index pages live in the donor's ibdata, and nothing in the
  recipient can merge them.
  - Rule: the donor runs `ibuf_merge_space()` on the selected spaces at start (as `row_quiesce_table_start`
    does), and requires `innodb_change_buffering=none` (the 8.4 default).
  - Otherwise the clone is refused.
- **Donor DDL on the selected tables**:
  - MDL_SHARED_READ on them for the whole clone, so DDL on them waits and other tables are not affected.
  - The SDI then matches the copied pages, and there are no file rename or drop cases for selected spaces.
  - The cost: an ALTER on a selected table waits for the clone.

### Step R: redo apply on detached files, with no buffer pool and no registered space
- **Pages**: work on private frames. `fil_iterate` / `fil_buf_block_init` already build a `buf_block_t`
  around a private frame for import's `PageConverter`.
- **Space ids**: the staged spaces keep their donor space_ids. The applier keeps a map
  donor space_id → staged file, so nothing collides with the recipient's fil_system and no id remap is
  needed during apply.
- **Parsing**: reuse the record parser from `recv_parse_log_rec` and the body parse/apply in
  `recv_parse_or_apply_log_rec_body`.
  - Both read the global `recv_sys` (`bytes_to_ignore_before_checkpoint`, `apply_file_operations`,
    `is_cloned_db`, `recovered_lsn`), so they need a context argument instead.
- **Batches**: hash (space, page) → records until a memory cap, then apply per page in LSN order.
  - A record applies only when its LSN is above the page LSN.
  - Records for other spaces are skipped.
- **File records**: MLOG_FILE_* for the selected spaces cannot appear, because of the MDL.
- **Encryption and compression**:
  - encrypted pages are decrypted and re-encrypted with the tablespace key, the way `fil_iterate` does;
  - the archived redo is decrypted with the key the clone apply already re-wraps;
  - ROW_FORMAT=COMPRESSED pages are applied through `page_zip`.
- **What it touches**: the selected spaces and the undo spaces, so that undo pages are current at the stop
  LSN.

### Step U: undo, a rollback limited to the selected tables
- **Which transactions**: those whose undo segment is ACTIVE or PREPARED at the stop LSN. These are the
  same transactions crash recovery would roll back. XA PREPARED counts as not committed.
- **Finding them**: from the rseg headers and undo segment headers in the staged undo files, read by a
  small detached reader. They are not added to the recipient's `trx_sys`.
- **Why rollback can't run on the detached files**: undoing an update can split a page, and undoing a
  change touches every secondary index. That needs real B-tree operations, so the table must be attached
  first.
  1. **Raw attach**: a mode of the refactored import that converts only the space_id, the page LSN and the
     checksum. It keeps DB_TRX_ID and DB_ROLL_PTR, and does no purge. The result is a hidden staging table
     (an `#sql`-style name), with its `dict_table_t` built from the SDI (`dd_build_uncached_table` already
     exists on the import branch).
  2. **Rollback**:
     - walk the undo logs of the open transactions newest first;
     - for each record whose table_id is a selected table, call the row_undo code (`row_undo_ins`,
       `row_undo_mod`) on the staging table.
     - The roll pointers point into the donor's undo, so the undo-record fetch (`trx_undo_get_undo_rec_low`
       and the previous-version builder) needs an "external undo" source. It reads the staged undo files
       through private frames, not through the recipient's rsegs.
     - The rollback writes the recipient's own redo. If the recipient crashes, log_ddl drops the staging
       table and the clone starts over.
  3. **Finish the import**: reset DB_TRX_ID and DB_ROLL_PTR, purge the delete-marked rows (now all
     committed), set the AUTO_INCREMENT, link the FKs, and rename to the real name in one DDL transaction.
     This is the `IMPORT TABLE FROM '.ibd'` attach path, cut into two halves.
- **Cost**: the rollback work grows with the open transactions at the end, as in crash recovery. The redo
  apply grows with the redo generated during the clone.

### Cheaper alternative to step U (for comparison)
- At the stop point, drain the writers of the selected tables: an InnoDB table S lock waits for IX holders,
  and new writers wait for the S lock to be released.
- No undo is shipped and there is no rollback code; step R plus a normal IMPORT is enough.
- The cost: a write stall on those tables while it waits for the oldest open writer, and an XA PREPARED
  writer blocks it until timeout.

### To check in code
- How much of `recv_parse_or_apply_log_rec_body` and the `mlog` parsers is pure, and how big is the
  context refactor?
- Can the row_undo paths run on a `trx_undo_rec_t` from an external source? Their callers assume
  `trx->rsegs`.
- Undo record format compatibility across 8.4 point releases. Clone allows different point releases.
- Undo truncation on the donor during the clone: an undo space that is truncated and recreated during the
  copy. Clone's `Clone_notify UNDO_DDL` handles this for full clones.
- The size of the undo tablespaces on the wire, when there is a long history list.

## 9. Redo via upstream `ib::redo::Redo_applier`; undo is the hard part (2026-10-17)

### Redo: WL#15560, "Standalone REDO log parsing and applying library"
- Commit f615971822e3, tag mysql-26.7.0, so it is in PXB 26.07 and 26.10. Header `log0redo.h`.
- **Interface**:
  - `Redo_applier::parse_mtr(span)` → `Mtr_view` of `Record_view`s (Page, Space, Table and Aux kinds, with
    space_id and page_no);
  - `apply(Record_handle, Page_handle&)` works on a plain frame, or a zip frame plus metadata, with no
    buffer pool.
- **What the caller still owns**:
  - no side effects: MLOG_FILE_* records, FSP size and encryption_op_in_progress are the caller's job;
  - clear text only: decryption and the log block and file headers stay outside.
- So partial clone needs only a driver around it:
  - read the archived redo blocks, strip the headers and decrypt;
  - for each mtr, take the Page records of the selected spaces (and of undo spaces, if undo is shipped);
  - group them by page, read the page from the staged file, apply in LSN order above the page LSN, write
    back.
- Parsing index-record types needs index descriptions (`mlog_parse_index`, cached since WL#17287).
- **8.4 has no such class.** The WL#15464 dependency claimed here earlier was not verified. See section 19
  for the actual check.

### Undo: two ways
**U1. Ship the undo, roll back on the recipient** (section 8, step U).
- Apply redo to the staged undo too, find the active transactions from the undo segment headers, attach
  the table raw, and run row_undo through an "external undo" record source.
- **Against**:
  - an undo-fetch abstraction inside the trx code;
  - the undo format across point releases;
  - undo truncation during the copy (WL#16989 changes truncation in 26.7);
  - undo size on the wire.

**U2. No undo: the donor fixes up the touched rows from a read view (proposed).**
- **Donor**: just before taking the stop LSN L, it opens a read view V and keeps it until the recipient
  is done.
  - Every transaction visible in V committed before V, so all its changes are at or below L and are in
    the staged pages.
- **Recipient**: during the page conversion (which already visits every clustered record to reset
  DB_TRX_ID), it collects the PK of each record whose DB_TRX_ID is **not visible in V**. The donor sends
  V's ids. That PK set covers:
  - rows inserted by an open transaction;
  - rows updated by one;
  - rows delete-marked by one;
  - both halves of a PK change;
  - work from transactions that committed between V and L.
- **Fix-up**:
  - the recipient sends the PK set to the donor;
  - the donor answers with each row as of V (a consistent read with V), or "absent";
  - on the hidden staging table, the recipient deletes or upserts those rows with normal row operations,
    so the secondary indexes and BLOBs are handled by existing code;
  - then the final rename.
- **The result** is exactly V's state, for every selected table at once. Partial clone gets
  `START TRANSACTION WITH CONSISTENT SNAPSHOT` semantics.
  - No commit block is needed.
  - Undo is never shipped or parsed.
  - Rows that are not touched are at their L version, which equals their V version.
- **Ordering with import's purge**: if purge drops a delete-marked row first, the upsert puts it back.
  So the order is safe, and no undo is needed for it either.
- **Costs**:
  - the donor's purge is held back by V until the fix-up ends;
  - the size of the fix-up grows with the rows changed by transactions open at L (a large uncommitted
    bulk DML means many rows sent);
  - one more protocol round trip;
  - tables without a PK need an internal lookup by DB_ROW_ID on the donor.
- **To verify**:
  - V visibility versus records written by transactions that started after V: their id is at or above the
    low limit, so they are invisible, which is correct;
  - the case where the donor has dropped or truncated the table: no, MDL blocks it.

## 10. Page tracking + a short lock at the end (Satya, 2026-10-17)

This is FLUSH TABLES ... FOR EXPORT, done incrementally: copy without a lock, then hold the lock only for
the last small round.

### Flow (donor)
1. **Start.** Take MDL_SHARED_READ on the selected tables for the whole clone, which blocks DDL on them
   only. Start page tracking (`Page_Arch_Client_Ctx::start`, or the page-track service: `page_track_start`).
   If the donor has change buffering on, merge the change buffer for these spaces (`ibuf_merge_space`).
2. **Round 0.** Copy each selected .ibd raw, as FILE_COPY does today.
3. **Rounds 1..n.** Get the page ids flushed since the previous round, filtered to the selected
   space_ids. `innobase_page_track_get_page_ids(start_id, stop_id)` already takes an id range; MEB uses it
   for incrementals. Send those pages. Repeat while the delta keeps shrinking, until it is below a
   threshold or a maximum number of rounds is reached.
4. **Final round, under a lock:**
   - **Writers**: take MDL_SHARED_NO_WRITE on the tables, the FOR EXPORT lock. It waits for the open
     writers' transactions to end, so no uncommitted change remains.
   - **Purge**: stop it (`trx_purge_stop`, as `row_quiesce_table_start` does). Purge would otherwise keep
     changing the B-tree under the lock.
   - **Flush**: flush the tables' dirty pages (`buf_LRU_flush_or_remove_pages(space,
     BUF_REMOVE_FLUSH_WRITE)`).
   - **Send the last delta**: pages tracked since round n, plus the file size and page 0. Optionally read
     them from the buffer pool under latch, as PAGE_COPY does, instead of flushing first.
   - **Release**: resume purge, release the MDL.
5. **Metadata.** Send what the import needs and the .ibd lacks: the AUTO_INCREMENT counter, triggers, the
   schema charset and collation, and the key for encrypted tables (as a .cfp).

### Recipient
- Writes the pages into the staged file. The clone apply already writes by offset.
- Then runs `IMPORT TABLE FROM '<db>/<t>.ibd'` (the attach path): it resets DB_TRX_ID and DB_ROLL_PTR,
  purges the delete-marked rows (all committed by now), and sets the space id and LSN.
- Then the `--restore` steps: AUTO_INCREMENT, FULLTEXT rebuild, triggers.

### Why it is consistent
- At the moment of the final flush there are no writers, no open writer transactions, no purge and no
  change buffer. The table's disk image is then a quiesced image, the same state FLUSH ... FOR EXPORT gives.
- Every page that changed since round 0 was flushed at least once after that change, so it is in some
  round's tracked set. The last round covers the final flush.
- No redo, no undo, and no redo applier.
- **Point in time**: all selected tables share the point at which the final lock is held, provided the
  final round takes one lock over all of them.

### Why the recipient needs no redo or undo
- **Redo** fixes pages that are older than the cut. Here every page the recipient holds is at its version
  at the cut:
  - pages unchanged since round 0 are current;
  - pages flushed since then are in a tracked id range and were re-sent;
  - pages still dirty at the cut are copied from the buffer pool under the lock.
  - Page 0, the extent descriptors and the inode pages are ordinary pages, so they are covered the same
    way, and so is the file size.
- **Undo** removes uncommitted changes. There are none at the cut, because the lock waited for every open
  writer.
- **No mini-transaction is half done across pages**, since no writer, purge or change-buffer merge is
  running on these spaces. Requirement: wait for the running purge batch to finish (`trx_purge_stop`
  does).
- **What is left are committed leftovers**: DB_TRX_ID and DB_ROLL_PTR pointing into the donor's undo, and
  delete-marked rows not yet purged. A FOR EXPORT file has the same leftovers, and import already handles
  them: `PageConverter` resets the system columns and purges delete-marked records.
- **So the recipient only writes the pages at their offsets, then runs IMPORT TABLE FROM.**

### How long writers are blocked
- **Lock wait**: taking SNW waits for the open writers' transactions. While it waits, new DML queues behind
  it in MDL.
- **Under the lock**: flushing the dirty pages of these tables (bounded by their dirty-page count), plus
  sending the last delta.
- **Bounding it**:
  - a timeout on the lock request; on timeout, release, run another round, and retry with a backoff;
  - stop the rounds only when the delta is small;
  - an option to copy the delta into memory under the lock and send it after the release.
- **Knobs**: something like `clone_partial_lock_timeout` and a maximum number of rounds. These need
  measurement.

### Open
- **Base line.** This needs no redo applier, so 8.4, where the import branch is, works again. Do we still
  move to 26.07?
- **Global purge stop.** Is it acceptable for that moment (FOR EXPORT already does it)? Or can purge be
  paused for the selected tables only?
- **Torn reads in round 0.** A raw read can only be torn by a concurrent page write. That write is tracked
  (pages already under write at start are forced into tracking, `Arch_Page_Sys` `set_tracking_buf_pool`), so
  the page is re-sent. This is the same argument as clone's FILE_COPY plus PAGE_COPY.
- **FTS aux tables.** Background FTS sync writes them. They are out of v1 anyway, since FULLTEXT is rebuilt.
- **Page tracking cost.** Page tracking is global: every flush is tracked while it is on. Clone already
  pays this cost.

## 11. Online export as the shared primitive (Satya, 2026-10-17)

### Problem
- FLUSH TABLES ... FOR EXPORT holds MDL_SHARED_NO_WRITE from the flush until UNLOCK TABLES. That covers
  the whole time the user copies the files, so writers are blocked for the whole copy.
- The single-table-restore design rejected it for that reason ("blocking writes is a no-go"). It left open
  a non-blocking way to produce importable files.
- Partial clone needs the same thing.

### Idea
- An export that the **server** drives: it does the copying itself. Only then can it run the tracked
  rounds and take the lock just for the last delta, as in section 10.
- The same engine writes either to a directory or to a clone stream:

```
              +--> directory sink: .ibd + .cfg/.cfp (+ metadata)  -> user, PXB, scripts
online export |
 (rounds +    +--> clone sink: COM_CLONE stream -> recipient stage -> IMPORT TABLE FROM '.ibd'
  short lock)
```

### Possible user syntax
Symmetric with `IMPORT TABLE FROM`:

```sql
EXPORT TABLE db.t1[, db.t2 ...] INTO '/dir'      -- or: EXPORT DATABASE db INTO '/dir'
```

- **Output**:
  - `<dir>/<db>/<t>.ibd` (+ partition files), importable with the new `IMPORT TABLE FROM '.ibd'`;
  - a .cfp for encrypted tables;
  - a .cfg, so the old `DISCARD` + `IMPORT TABLESPACE` keeps working;
  - possibly a small metadata file (AUTO_INCREMENT, triggers, schema charset) until those live in the SDI
    (the "the .ibd carries everything" future item in the PXB DESIGN.md).
- **The directory**: like SELECT ... INTO OUTFILE, subject to `secure_file_priv` and FILE. Or only under
  the datadir.
- Every tracked table gets **one point in time**: the final lock is taken over all of them at once.

### Users
- **Users and scripts**: an online replacement for FLUSH ... FOR EXPORT + cp.
- **PXB**: a fast restore point that is ready to import without blocking writes. This is the undecided
  item in the single-table-restore DESIGN.md.
- **Partial clone**: `CLONE TABLE/DATABASE ... FROM donor` runs the same engine on the donor with the
  clone sink, then `IMPORT TABLE FROM` on the recipient.

### Engine pieces (InnoDB)
- **Begin**: MDL_SHARED_READ for the whole export, start page tracking, merge the change buffer.
- **Copy**: round 0 is the raw file copy, then the tracked rounds over the page id range.
- **Final**: take SNW over all the tables, stop purge, flush (or read from the buffer pool), take the last
  delta, write the .cfg/.cfp, release.
- **Reuse**: `row_quiesce_*` already holds the quiesce and .cfg writer. The import branch already
  refactored the .cfg writer (`Cfg_write_source`). The arch page tracking already supports ranges.

### Open
- **Order of delivery**: EXPORT first (it is useful by itself and testable without clone), then the clone
  sink?
- **Statement form**: a new `EXPORT TABLE ... INTO`, or an online mode of FLUSH ... FOR EXPORT? FLUSH
  cannot do the rounds, because the user copies the files, not the server.
- **Base line**: 8.4 or 26.07. Nothing here needs the redo applier.

## 12. Online FLUSH TABLES ... FOR EXPORT with a delta file (focus as of 2026-10-17)

### Today
- `FLUSH TABLES t FOR EXPORT`:
  - takes MDL_SHARED_NO_WRITE;
  - `row_quiesce_table_start` stops purge, merges the change buffer, flushes the space, and writes
    `t.cfg` (plus `t.cfp` if encrypted);
  - the user copies `t.ibd`, `t.cfg` and `t.cfp`;
  - `UNLOCK TABLES` runs `row_quiesce_table_complete`, which deletes the .cfg/.cfp and resumes purge.
- Writers are blocked for the whole copy.

### Proposed flow (user still copies with cp)

```
FLUSH TABLES t FOR EXPORT ONLINE;   -- returns at once: MDL_SHARED_READ (DDL waits, DML runs),
                                    -- page tracking on for t's spaces, export marker set
  cp t.ibd ...                      -- a fuzzy copy, taken while DML runs
UNLOCK TABLES;                      -- short final step (below) writes t.cfg, t.cfp, t.delta
  cp t.cfg t.cfp t.delta ...
```

### The final step at UNLOCK
- **Before the lock**: a background pass appends to `t.delta` the pages flushed since the last pass,
  using page-track id ranges. This can also run in rounds while the user copies.
- **Under a short MDL_SHARED_NO_WRITE**:
  - wait for the open writers to end, then stop purge (wait for the running batch);
  - append the last tracked pages and the pages still dirty in the buffer pool, copied from the buffer
    pool under latch;
  - record the final file size;
  - write the .cfg/.cfp, as today but at the end;
  - release.
- **Bounding the wait**: a lock timeout, with retry after another background pass, so a long transaction
  never stalls DML for more than the timeout.

### The delta file
- An append-only log of full page images (`space_id, page_no, page image` as on disk, so still encrypted
  or compressed), plus a header and a trailer.
- **The last entry for a page wins.**
- **The trailer** holds the final file size, the number of pages, a checksum, and the export id.
- **Size**: every page changed while the copy ran, at most the size of the table. Hot tables give big
  deltas, so the rounds don't reduce its size; they only shorten the time under the lock.

### Import, with no redo and no undo
- **Before the page conversion**: overwrite the pages in the copied .ibd with the delta images (last one
  wins), then set the file size.
- The result is the quiesced image of the cut, the same as an old FOR EXPORT file. Then the normal
  `PageConverter` runs: system columns reset, delete-marked rows purged, and so on.
- Both `ALTER TABLE ... IMPORT TABLESPACE` and `IMPORT TABLE FROM '.ibd'` get the step: one helper before
  `row_import_convert()` on the import branch.

### Safety: a fuzzy .ibd must never be imported without its delta
- An **export marker in the live tablespace's page 0** (a redo-logged header field):
  - set at FLUSH ... ONLINE, so every fuzzy copy carries it;
  - cleared at the end, and the delta's page 0 image is the cleared one.
- **Import** refuses a file with the marker and no matching delta (same export id), so a forgotten delta
  is an error, not silent corruption.
- **Older servers** don't know the marker. Make it a field an older import rejects, for example an fsp
  flag bit (to check), so an old server fails too.

### Making import easier (the same export writes it)
- The .cfg is written at the end, so it matches the cut. The .cfp holds the transfer key, as today.
- Missing today, and needed by `xtrabackup --restore` / IMPORT TABLE FROM:
  - the AUTO_INCREMENT counter;
  - the triggers;
  - the schema charset and collation.
  - Either put them in a small metadata file, or (the PXB DESIGN.md "the .ibd carries everything" future
    item) refresh the SDI at export: AUTO_INCREMENT in the table SDI's se_private_data, and the triggers
    and schema defaults as an SDI record of a new type.
  - The SDI refresh would be part of the final step and land in the delta's pages.
- Then `IMPORT TABLE FROM 't.ibd'` alone recreates everything from `t.ibd` + `t.delta` (+ `t.cfp`).

### Open
1. **Syntax**: `FOR EXPORT ONLINE`, a new option, or a sysvar that makes FOR EXPORT online?
2. **Lifetime of the files**: today .cfg/.cfp are deleted at UNLOCK, but online mode creates them at
   UNLOCK. Keep them until the next export or a DROP? Or write them to a directory the user names?
3. **Multiple tables**: one cut over all the tables in the statement, as FOR EXPORT gives today.
4. **Purge**: is a brief global stop at the final step acceptable (FOR EXPORT stops it already), or should
   purge pause per table?
5. **The marker**: which header field is safe and rejected by older servers?
6. **Partitions**: one delta per partition file, or one per table?
7. **PXB**: could xtrabackup drive the same thing? Reading the tracked pages itself is the
   `--page-tracking` incremental feature, and the delta format could be shared.

## 13. Cleaner alternatives to "UNLOCK writes t.delta"

The delta exists only because the user copies a fuzzy .ibd outside the server's control. There are two
ways to remove the odd part.

### E1. The server makes the copy: `EXPORT TABLE t[, ...] INTO '/dir'` (one statement)
- **Copy**: the server copies t.ibd into `/dir` raw, while DML runs, with page tracking on.
- **Rounds**: the pages flushed since the previous round are rewritten **in place in the target copy**.
  The target is a file the server owns, so there is no delta.
- **Final step, under a short lock**:
  - wait for the open writers, pause purge;
  - write the last pages, including the ones still dirty in the buffer pool, into the target;
  - set the size, write the .cfg/.cfp (and the extra metadata);
  - release.
- **Output**: plain, consistent export files. Any import works, including on older servers and the
  existing `IMPORT TABLESPACE`. There is no marker, no new import step and no delta format.
- **Costs**:
  - the files land on the server host, so FILE privilege and `secure_file_priv` apply, as for
    SELECT ... INTO OUTFILE;
  - the server needs disk space for a full copy;
  - nothing is streamed to a remote host. That is where clone or PXB comes in later, with the same engine.

### E2. Keep the user's cp, and do the final step as a normal FOR EXPORT
```
FLUSH TABLES t FOR EXPORT ONLINE;  -- no write lock: page tracking starts, DDL waits
  cp t.ibd ...                     -- fuzzy
FLUSH TABLES t FOR EXPORT;         -- the normal statement: short write lock; writes t.cfg (+ t.cfp)
                                   -- and t.delta = the pages changed since ONLINE
  cp t.cfg t.cfp t.delta ...       -- under the lock; t.delta = every page changed during the cp
UNLOCK TABLES;                     -- as today: deletes the files, resumes purge
```
- **For**: UNLOCK keeps its meaning. The files are written by the FOR EXPORT statement, as today.
- **Against**:
  - the lock also covers writing and copying the delta, which can be large for a hot table;
  - the delta format, the page-0 marker and the import step from section 12 are still needed.

**My view**: E1 is cleaner. Import needs no change, and the files are the format everybody already reads.

## 14. A target that is not local: an append-only format and a completion marker

### Effect on the design
- An S3 object or a stream can't be rewritten in place, so E1's "rewrite the changed pages in the target"
  works only for a local directory.
- For any sink, the export becomes **append-only**:
  - the base copy of each .ibd;
  - then page records (`space_id, page_no, image`, last one wins) from the rounds and the final step;
  - then .cfg, .cfp and the metadata;
  - then a manifest.
- The delta comes back, but as an **internal part of one export unit** that the server writes in one
  statement. It is not a loose extra file the user has to remember.
- **The reader** (import, or a small "materialize" tool) rebuilds the plain .ibd: base, then page records
  in order, then the final size.

### Sinks
- An `Export_sink` interface: `create(object)`, `append(bytes)`, `finish(object)`, `commit()`, `abort()`.
- v1: a local directory. Later: S3 (the PXB xbcloud code is prior art), and maybe a stream to a client or
  the clone transport.
- **A local directory can also materialize plain files** at commit: apply the page records in place and
  drop them. Older servers' IMPORT TABLESPACE can then read the result. Open: always do it locally, or
  keep one format everywhere?

### Telling interrupted from complete
- **Rule: the manifest is written last, and only its presence means complete.**
- **Local directory**:
  - write everything under a temporary name (`<dir>/.<export_id>.tmp/`) and fsync each file;
  - write `manifest.json` and fsync it;
  - rename the directory to its final name and fsync the parent.
  - A crash leaves only the temporary directory, which the next run or a cleanup removes.
- **S3**: upload the objects, then PUT `manifest.json` last. A single PUT is atomic. No manifest means
  incomplete, and a lifecycle rule or the next run cleans up.
- **A plain byte stream (pipe)**:
  - frame everything in chunks: `{magic, export_id, seq_no, object, offset, len, crc32c}`;
  - the last chunk is a trailer with the total chunk count, the total bytes and a whole-stream checksum;
  - a reader that hits EOF without a valid trailer, or finds a gap in seq_no, reports "incomplete".
  - This is the xbstream model.
- **What the manifest holds**:
  - format version, export id (UUID), server_uuid and version;
  - the cut (time; LSN for information);
  - per table: the objects, sizes and checksums, page counts and size after rebuild;
  - .cfg/.cfp present or not, and the metadata;
  - a checksum over the manifest itself.
- **Import or the reader**:
  - refuses an export with no manifest, a bad manifest checksum, or a missing or short object;
  - checks each object's checksum as it reads;
  - checks the page checksums after the rebuild.
- **Failure on the server**: the statement fails, `abort()` removes what it can, and no manifest is ever
  written. A server crash in the middle leaves no manifest either.
- **Resume after an interrupted export**: not in v1. The export runs again from the start, as the page
  tracking would have to restart anyway.

### Prior art to reuse
- **xbstream**: chunk framing and checksums.
- **xbcloud**: S3 upload, retries, and partial-upload handling.
- **PXB 9.7 manifest proposal** (`backup_metadata.json` + `backup_files.jsonl`, in the single-table-restore
  STATUS.md and DESIGN.md). Share the manifest shape with it if that lands.

## 15. Export writes the metadata into the SDI of the .ibd

### The SDI today
- A B-tree in each file-per-table .ibd, keyed `(type, id)`.
- Types used: `SDI_TYPE_TABLE = 1` (sql/handler.h) and 2 (tablespace).
- **The table SDI** serializes the `dd::Table` (`Table_impl::serialize`): columns, indexes, FKs, check
  constraints, partitions, and `se_private_data`.
  - That data already holds what the import needs: index ids and roots, the instant columns and row
    versions, and the `autoinc` key (`DD_TABLE_AUTOINC`).
  - That is why `IMPORT TABLE FROM '.ibd'` (Import_sdi_source) can build the import metadata **without a
    .cfg**.
- **What is missing**:
  - **triggers**: `Table_impl::serialize` has no triggers;
  - **schema defaults**: there is no schema SDI in the .ibd;
  - **a current AUTO_INCREMENT**: the SDI is rewritten only on DDL, so its `autoinc` is stale.

### Proposal
- In the final step, under the lock, the export **refreshes the SDI of the live tablespace** with the
  normal, redo-logged SDI API (`sdi_set`):
  1. the table record (type 1), with the current AUTO_INCREMENT in se_private_data;
  2. a **new record type 3**, "export info": triggers (definition, definer, sql_mode, charsets, order),
     schema charset, collation and default encryption, and the export id and time.
     - Keeping it in a separate type leaves the type 1 record upstream-compatible.
- The SDI pages changed by this are ordinary dirty pages of the tablespace, so they **leave with the last
  delta** with no extra mechanism.
- The exported .ibd is then self-describing. `IMPORT TABLE FROM '.ibd'` can create the table, set
  AUTO_INCREMENT, create the triggers and create a missing schema, all from the file.
- **The export unit becomes**:
  - the .ibd (base copy + page records);
  - the .cfp, if encrypted;
  - the manifest.
  - **No .cfg** for the new import.
- **The .cfp stays outside the SDI.** The tablespace key can't be stored in clear text inside the file it
  protects, and the SDI pages of an encrypted tablespace are encrypted with that same key. Later, the key
  could go into the manifest, wrapped with a user-supplied export password or the target's keyring.

### Questions
- **Leftovers in the live file**:
  - the refreshed SDI and the type 3 record stay in the donor's .ibd;
  - the next export overwrites them;
  - they are stale in between, which is harmless because DD is the source of truth on the donor;
  - should DROP TRIGGER or ALTER SCHEMA remove or refresh them? Probably not: only export reads them.
- **Old servers and tools**:
  - Do ibd2sdi and older IMPORT handle an unknown SDI type? `dict_sdi_get_keys` returns every key. Check
    that `IMPORT TABLE (.sdi)`, ibd2sdi, PXB's `dict_load_from_spaces_sdi` and clone ignore type 3.
- **Size**: tables with many large triggers. SDI records are compressed and use BLOB pages, so this is
  probably fine.
- **Partitions**: the table SDI lives only in the first partition's .ibd. Put type 3 in the same place.
- **The old path**: `ALTER TABLE ... IMPORT TABLESPACE` still wants a .cfg for instant columns. Write the
  .cfg only for a local target, for compatibility, or drop it?

## 16. Partial clone = filtered FILE_COPY + PAGE_COPY rounds + a short final lock + IMPORT

The recipient's stage is a local file that the clone apply already writes **in place by offset**. So for
clone there is no delta file and no append-only format: the staged .ibd becomes a plain, consistent file,
and `IMPORT TABLE FROM '.ibd'` takes it as it is.

### Mapping onto the clone states (donor)
| State | Today | Partial clone |
|---|---|---|
| INIT | open every DD tablespace | resolve the names to space_ids, take MDL_SHARED_READ on the selected tables for the whole clone (blocks DDL on them only) |
| FILE_COPY | raw copy of every space, page tracking on | `add_node` keeps **only the selected spaces**: no ibdata, undo, mysql.ibd, redo or buffer-pool dump file |
| PAGE_COPY | send the tracked pages from the buffer pool (SX latch), once | the same, **repeated in rounds** over page-track id ranges while the delta shrinks |
| REDO_COPY | redo archive, end LSN, GTID and binlog position | **replaced by a FINAL state**, below |
| DONE | | |

### The FINAL state, under the short lock
1. **Lock**: take MDL_SHARED_NO_WRITE on **all** selected tables in one request, so they share one cut.
   - A timeout applies. On timeout, release, run another PAGE_COPY round, and retry with a backoff.
   - Waiting for SNW also waits for the open writer transactions, so none remains.
2. **Purge**: stop it and wait for the running batch (`trx_purge_stop`). Merge the change buffer for these
   spaces if it is in use.
3. **SDI**: refresh the SDI (section 15): the current AUTO_INCREMENT, plus the type 3 export info
   (triggers, schema defaults).
4. **Collect the last delta**:
   - pages tracked since the last round;
   - all pages still dirty in the buffer pool for these spaces (a flush-list scan; no flush to disk);
   - the file sizes and page 0.
   - **Copy them into memory** (or a temporary file if large) under page latches.
5. **Release** the MDL and resume purge, **then** send the copied delta.
   - DML is blocked only for steps 1-4.
   - A network failure never holds the lock. Without this, `clone_donor_timeout_after_network_failure`
     could keep SNW for up to 5 minutes.

### Recipient
- **Stage**: under the datadir (`#clone_stage/<id>/<db>/<t>.ibd`). The existing apply code writes FILE_COPY
  data, then each round's pages, then the final delta, all in place.
- **Then**, for each table:
  - check that the target table doesn't exist;
  - rename the staged file to `<datadir>/<db>/<t>.ibd`;
  - run `IMPORT TABLE FROM` (the attach path). Several tables can go in one statement, all or none.
- **Triggers, schema and AUTO_INCREMENT** come from the SDI type 3 record. No separate metadata exchange is
  needed.
- **Failure or crash**: remove the stage at startup. IMPORT is atomic and log_ddl removes converted files,
  so no half tables remain.
- **Encryption**:
  - Clone already re-wraps page 0's key with the recipient's master key, and SSL is forced.
  - So the import can read the key from page 0 with the recipient's own master key, with no .cfp. That
    is a small extension of the attach path.
  - The alternative is for the recipient to write a .cfp itself.
- **Binlog and replication**: as IMPORT TABLE, not binlogged, local only.

### What changes in clone
- **Protocol**: COM_INIT gets an object list. That is a protocol bump after trunk's 0x0103, or 0x0103 on
  8.4 if this is 8.4-only. The FINAL state needs a new state descriptor.
- **Snapshot**:
  - the filter in `add_node`;
  - a round loop in PAGE_COPY;
  - the FINAL state instead of REDO_COPY;
  - no `init_redo_archiving` and no GTID or binlog persistence.
- **`Clone_notify`**: must ignore DDL on spaces that are **not** in the snapshot. Today
  `begin_ddl_state` can block or abort any DDL during a clone.
- **Recipient**:
  - no `clone_drop_user_data`, no backup lock, no restart;
  - no `#clone` replace files and no `clone_files_recovery`;
  - instead: stage, rename, IMPORT.
- **Checks**: the version series and page size only, not every plugin and config (`send_params`).
- **Restart after a network failure** (COM_REINIT) still works per chunk in FILE_COPY and PAGE_COPY. In
  FINAL, the lock is already released by then, and the delta is in memory or a temporary file and can be
  resent.

### Open
1. **Base line**: nothing needs the redo applier, so 8.4 (where the import branch is) works. 8.4 or
   26.07?
2. **Global purge stop** during the FINAL steps 1-4: OK?
3. **Round policy**: the threshold for "small enough" (pages or bytes), the maximum number of rounds, the
   lock timeout and the backoff. New sysvars, or reuse `clone_ddl_timeout`?
4. **One clone at a time** (`Clone_handler`): keep that for partial clones?

## 17. A solid design: a copy-on-write snapshot with the short lock at the start (proposal)

### The idea
- Take the short lock at the **beginning**, to define a cut that has no uncommitted changes. Then copy
  online in **one pass**.
- Pages modified after the cut keep their cut-time image (copy-on-write), the way an LVM snapshot or a SQL
  Server database snapshot does.
- **No rounds, no page tracking, no redo, no undo.** The lock time does not depend on the copy time or on
  the write rate.

### The cut (short lock)
1. Take MDL_SHARED_NO_WRITE on all selected tables in one request. That waits for the open writers.
2. Stop purge, wait for the running batch, and merge the change buffer for these spaces (or require it to
   be off).
3. Refresh the SDI (section 15): the current AUTO_INCREMENT, triggers and schema defaults.
4. Set `cut_lsn = log_get_lsn()`, record each space's size at the cut, and activate the snapshot on these
   spaces: `fil_space_t::m_snapshot`, a pointer that is null when inactive.
5. Resume purge and release the MDL. Writers, purge and DDL-free work then run normally.

At the cut no mini-transaction on these spaces is in flight and no change is uncommitted. The image "every
page as of cut_lsn" is therefore a quiesced, importable image: what FLUSH ... FOR EXPORT gives.

### Copy-on-write (the hook)
- **Rule**: before a mini-transaction modifies a page of a snapshot space whose LSN is at or below
  `cut_lsn`, save the page's current image as its **pre-image**. That image is exactly the cut version.
  This happens at most once per page: once modified, the page LSN is above the cut.
- **Where**:
  - when a page of a snapshot space is X- or SX-latched for modification: `buf_page_get_gen` and the other
    latch-taking getters (optimistic, known-nowait, try);
  - in `buf_page_create`, where a page is allocated after the cut. It was free at the cut, and import
    treats it as free, so the pre-image is a marker "free at cut" that the reader sends as a zero page.
- **Cost on the hot path**: one null-pointer test on the space for every page, everywhere. The pre-image
  copy (16K) happens once per page changed during the copy, and only on snapshot spaces.
- **Store**: a temporary file plus a map `(space, page) → offset`. Memory or disk grows with the number of
  pages changed during the copy, and at most with the table size. A limit makes the export fail cleanly.
- **Debug check**: at mtr commit, any modified page of a snapshot space whose previous LSN was at or below
  the cut must have a pre-image. This proves the hook covers every path.

### The reader (single pass, in page order, at most up to the size at the cut)
For each page_no:
1. If a pre-image exists, send it.
2. Else, if the page is in the buffer pool: S-latch it.
   - If its LSN is at or below the cut, copy the frame (the cut version, even if dirty).
   - Else a pre-image must exist: go to 1.
3. Else read it from disk.
   - If the LSN is at or below the cut and the checksum is good, it is the cut version: a page not in the
     buffer pool has all its pre-cut changes flushed.
   - If the LSN is above the cut, or the page is torn by a concurrent write, the page was changed after
     the cut, so its pre-image exists: go to 1.
4. Prepare the page for output the way clone's `get_page_for_write` does: checksum, encryption,
   compression.

The output is the exact cut image, **written once, in order**.

### Effect on the earlier sections
- **One format for every sink**: a single ordered pass means the target simply receives a plain .ibd. The
  append-only delta of section 14 is not needed: a local directory, S3 or a stream all get plain files plus
  the manifest. The completion rules of section 14 (manifest last) stay.
- **Clone (section 16)**:
  - FILE_COPY reads through this reader;
  - PAGE_COPY rounds and the FINAL state go away;
  - states: INIT (with the cut), FILE_COPY, DONE;
  - the recipient writes the pages and runs IMPORT.
- **EXPORT TABLE ... INTO**: the same engine with a different sink.
- **Snapshot lifetime**: it ends when the reader finishes or the statement fails. Then the hook turns off
  and the store is dropped. A server crash simply loses the snapshot; nothing is persistent.

### Risks and questions
1. **Hook coverage**: audit every path that changes a page frame without going through the getters, for
   example code that X-latches `block->lock` directly. The debug check above finds the gaps.
2. **Pages changed outside a mini-transaction**: the page cleaner only writes, so it is fine. Check the AHI,
   the change buffer, and bulk-load (`btr0mtib`) paths. Bulk load writes no redo and may bypass the
   getters. MDL blocks ALTER on these tables, so their bulk paths can't run.
3. **Store size on hot tables**: worst case, a full second copy. A sysvar caps it.
4. **Purge**: stopped only during the cut. After the cut its changes are captured like any write.
5. **Multiple concurrent snapshots**: one per space, or a list? v1: one snapshot per space at a time.

## 18. Only existing mechanisms: clone as it is + a short drain at the end + Redo_applier + IMPORT

Every piece below exists today: clone's copy and redo archive, the 26.07 `Redo_applier`, and IMPORT. The
only new behaviour is the short drain of writers at the end, so that the end LSN carries no uncommitted
change. With that, undo is not needed.

### Donor: the clone states as they are, filtered
1. **INIT**: resolve the tables, MDL_SHARED_READ on them for the whole clone (DDL on them waits).
2. **FILE_COPY**: as today, with page tracking on. Fuzzy raw copy of **only the selected .ibd files**.
3. **PAGE_COPY**: as today, **once**. Start redo archiving, stop page tracking, send the tracked pages.
4. **REDO_COPY**, with a short lock at the point where today's code fixes the end LSN:
   - take MDL_SHARED_NO_WRITE on all selected tables. It waits for the open writers' transactions;
   - stop redo archiving: this is the **end LSN**;
   - release the MDL at once.
   - The lock covers only "wait for open writers + read an LSN". Nothing is sent under it.
   - Then send the archived redo, as today. Optionally the donor filters it with `parse_mtr` down to the
     records for the selected spaces.

### Recipient
1. The stage gets the fuzzy .ibd files plus the tracked pages, as clone writes them today.
2. **Redo apply** with `ib::redo::Redo_applier`:
   - parse the mtrs up to the end LSN;
   - for each page record of a staged space, apply it to the page read from the staged file when its LSN
     is below the record's;
   - write the page back.
   - This is what crash recovery does, limited to these files, through the upstream class.
3. **Result**: the files as of the end LSN. Redo holds only complete mtrs, so no B-tree change is half
   done. The drain guarantees no uncommitted change of these tables at that LSN.
4. Committed leftovers (DB_TRX_ID, DB_ROLL_PTR, delete-marked rows) are handled by IMPORT, as for any FOR
   EXPORT file.
5. Rename into `<db>/<t>.ibd` and run `IMPORT TABLE FROM`.

### What is new code (no new mechanism)
- The table filter in `add_node`.
- The drain lock around the end-LSN point in `init_redo_copy`.
- A driver for `Redo_applier` on the recipient: read the redo, decrypt, group by page, page I/O on the
  staged files.
- The IMPORT call at the end.

### Points to check
- **Change buffer**: buffered changes for these spaces sit in the donor's ibdata, which is not shipped.
  Merge them at INIT and require `innodb_change_buffering=none` (the default), or refuse.
- **Purge** may be in the middle of its work at the end LSN. That is fine: its mtrs are complete in redo,
  and the leftovers are import's job. No purge stop is needed.
- **The end LSN** lands at an mtr boundary, as for a full clone's recovery.
- **Encryption**: the redo key and page 0 are already re-wrapped by clone.
- **Base line**: 26.07 (Redo_applier). This matches the earlier decision.

## 19. A redo applier for 8.4

### What exists in 8.4
- **Recovery**: `recv_parse_log_recs` → `recv_single_rec` / `recv_multi_rec` → `recv_add_to_hash_table`,
  then `recv_apply_hashed_log_recs` → `recv_recover_page`, through the buffer pool. Parse and apply are fused
  in the static `recv_parse_or_apply_log_rec_body`, which reads the global `recv_sys` in several places.
- **MEB-only code**, `#ifdef UNIV_HOTBACKUP`: `meb_apply_log_record(recv_addr, block)` and
  `meb_apply_log_recs_via_callback`. It applies hashed records to a **block the caller supplies**. This is
  prior art for "apply to my own page", but it is not compiled into the server.
- **PXB 8.4 has no applier class.** `--prepare` runs InnoDB's normal recovery inside the xtrabackup
  process, with the global `recv_sys` (`redo_log.cc` calls `recv_parse_log_recs()`). That works because
  xtrabackup is its own process with its own InnoDB. A running mysqld can't do it.

### WL#15560 on 8.4: checked
- The commit (f615971822e3, 33 files, +2573/−1819) was applied with `git apply --check` on this 8.4 tree.
  **13 files fail**, including:
  - `log0recv.cc` (−1976/+ lines: recovery is rewritten on top of the new class);
  - `fil0fil.cc/.h`, `log0recv.h`, `page0zip.h`, `ut0dbg.h`, `log0ddl.cc`, CMakeLists;
  - tests and messages.
- The new files `log0redo.cc` and `log0redo.h` themselves only include headers that 8.4 has (btr0cur,
  buf0buf, fsp0fsp, mtr0log, page0zip, ...). **No WL#15464 dependency was seen.**

### Options for 8.4
- **P1. Full backport** (recovery moves onto the class, as upstream did).
  - **For**: one code path, and the same as 26.x.
  - **Against**: it rewrites crash recovery in an LTS. The risk is high for a feature that only needs a
    side path.
- **P2. Port the class side by side (recommended).**
  - Add `log0redo.cc/.h` as a new, standalone `ib::redo::Redo_applier`, plus the small helper changes it
    needs (page0zip, page0cur, fil redo helpers).
  - **8.4 recovery stays untouched.**
  - **For**: zero risk to crash recovery, the same API as 26.07, and the clone code is identical on both
    lines.
  - **Against**: in 8.4 the parse and apply code exists twice, the old fused one and the new one, so a
    redo-format fix must be made in both places. That is acceptable, because the redo format is frozen
    within 8.4.
  - **Proof**: upstream's unit test `log0redo-t.cc` comes along. Also add a test that replays a real
    recovery's redo through both paths and compares the pages.
- **P3. Enable the MEB path** (`meb_apply_log_record`) in the server.
  - **For**: little code.
  - **Against**: it is built around the global `recv_sys` and its hash table, the MEB-only code is not
    tested in server builds, and its API differs from 26.07.

### Proposal
P2 on 8.4, and upstream's class as it is on 26.07. The partial clone driver (read archived redo, decrypt,
group by page, apply to the staged files) is written once against the `Redo_applier` API.

## 20. A hand-written single-tablespace redo apply on 8.4 (sketch)

The applier is a small driver around two existing pieces: recovery's record parse/apply functions, and
import's file iterator.

### What we reuse
- **Record parsing**: `recv_parse_log_rec(&type, ptr, end, &space_id, &page_no, &body)`. It is already
  non-static in log0recv.cc. It calls `recv_parse_or_apply_log_rec_body(..., block = nullptr, ...)` in
  parse-only mode.
- **Record apply**: the same `recv_parse_or_apply_log_rec_body(type, body, end, space_id, page_no, block,
  mtr, ...)` with a block. It is static today, so it needs a non-static wrapper.
- **Page I/O on a file that is not attached**:
  - `fil_tablespace_iterate()` + a `PageCallback`, which is what IMPORT's `PageConverter` uses;
  - it reads every page into a private `buf_block_t` (`fil_buf_block_init`), decrypts and decompresses,
    calls the callback, then checksums, encrypts and writes back;
  - the import branch already split this into reusable steps (`row_import_convert`).
- **Log blocks**: the header, checksum and data-length helpers (`log_block_get_*`), and redo decryption
  with the key clone already re-wraps (`encrypt_key_in_log_header`).

### The driver
1. **Parse.** Scan the archived redo from its checkpoint LSN to the end LSN, block by block. Keep only
   complete mtrs (the multi-record end marker, or the single-record flag), as `recv_single_rec` and
   `recv_multi_rec` do.
2. **Filter and store.** For each record whose space_id is a staged space, store
   `(page_no → [start_lsn, end_lsn, type, body bytes])` in a hash.
   - Skip records of other spaces.
   - Skip MLOG_WRITE_STRING encryption info on page 0, as cloned-db recovery does.
   - Skip MLOG_TABLE_DYNAMIC_META: AUTO_INCREMENT comes from the SDI.
   - MLOG_FILE_* records and MLOG_INDEX_LOAD can't occur for these spaces, because MDL blocks DDL.
     Assert, and error out if one appears.
   - Memory is capped: when the cap is hit, apply the batch and continue, as recovery's batches do.
3. **Apply.** Extend the staged file to the largest page number referenced or to FSP_SIZE. Then run
   `fil_tablespace_iterate` with an `Apply_redo_callback`. For every page that has pending records:
   - if the record's start LSN is at or above the page LSN, apply it with the body function on the private
     block, using a dummy mtr in MTR_LOG_NONE (as `recv_recover_page` does);
   - set the page LSN to the end LSN.
   - The iterator writes the page back.
   - Optional: iterate only the pages in the hash rather than the whole file.
4. **Done.** The staged file is as of the end LSN, and IMPORT's own `PageConverter` pass follows.

### Points to handle
- **Global `recv_sys`**: the body function reads `recv_sys->bytes_to_ignore_before_checkpoint`,
  `apply_file_operations`, `is_cloned_db` and `recovered_lsn`.
  - At runtime the `recv_sys` object still exists (`recv_sys_free` empties it, `recv_sys_close` deletes it
    at shutdown), so the reads don't crash.
  - But the values are recovery's, not ours. Pass a small context argument instead, or filter the
    affected record types before the call.
  - Concurrent use: one apply at a time, or the context argument.
- **Debug asserts in the apply paths** (mtr memo, block lock): give the private block an initialized
  `lock`, X-latch it during the apply, and check which asserts depend on `recv_recovery_on`.
- **Record types that need an index description** (insert, delete, update): `mlog_parse_index` builds a
  temporary dict_index_t per record. It is slow but correct, as in 8.4 recovery.
- **ROW_FORMAT=COMPRESSED**: recovery applies to both the frame and `page_zip`. Check that the private
  block from `fil_iterate` has both. Otherwise refuse compressed tables in v1.
- **Pages created after FILE_COPY**: they are beyond the copied size, so extend with zeros. Their init
  record (MLOG_INIT_FILE_PAGE2 or the page create records) comes first in LSN order.

### Tests
- The applier must give the same pages as crash recovery. Take a debug-built server, run DML, kill it,
  recover it once normally and once by applying its redo to a copy of the .ibd with the applier, then
  compare the pages. Kill with no open transactions, or recover with innodb_force_recovery=3 (no
  rollback), so that both sides stop at redo apply.
- MTR tests with a concurrent write load, through the clone path, checking with CHECK TABLE and row counts
  and checksums against the donor at the cut.

## 21. Page-only transfer (no redo): when does the stream stop?

This is pre-copy live migration. Pages are sent while DML runs; the stream
cannot catch up with a table that changes faster than the link sends it.

- The final lock must capture every page changed since its last send. That
  backlog is bounded by the number of distinct hot pages (worst case: the
  whole table), not by the DML rate.
- Under the lock the backlog only has to be copied locally (BP memory, or a
  disk read into a spill file). The network send happens after unlock.
- Stop rule (no convergence guarantee): take the lock once the backlog falls
  below N pages, or after a max wait. When the max wait expires, either
  take the lock anyway (a long lock) or fail the clone.
- Under continuous DML on a hot table: the backlog stays near the hot set,
  so the lock costs one local copy of the hot set.

Compared with redo (section 18): with redo the lock is only the writer drain,
independent of the workload; redo shipping happens after unlock.
Page-only is simpler on the recipient, but its lock time depends on the workload.

## 22. 8.4: does redo apply on a live server mean reinventing recovery?

No new apply logic is needed. Each redo record is applied by the functions crash recovery already
calls (`page_cur_parse_insert_rec`, `page_cur_parse_delete_rec`, `mlog_parse_*`, `page_zip_parse_*`,
`btr_cur_parse_*`). Upstream's `log0redo.cc` (976 lines) calls the same functions; it is a
reorganised `recv_parse_or_apply_log_rec_body` plus a loop. The only new code is the driver: read
records, keep those of our spaces, apply each to a page.

Isolation on the live server:
- The pages are private blocks of a file that is not attached (`fil_tablespace_iterate`). No buffer
  pool, no fil_system entry, no `recv_sys` (with P2), no redo is written.
- A bug can only damage the staged file. IMPORT then refuses it, or CHECK TABLE flags it; the
  server's own data is never touched.

8.4 choices:
- P2: port upstream's `Redo_applier` side by side (section 19). The code is ported, not written,
  and it is the same API as 26.07.
- Section 21 (pages only) on 8.4, redo on 26.07: no applier on 8.4, but the lock depends on the
  workload, and two flows must be maintained.
- Partial clone only on 26.07+.

## 23. Locked design (2026-10-18)

Sections 18 (flow), 20 (8.4 driver) and 22 (isolation) are the design. Everything else is background.

### Donor: clone states as today, filtered to the selected tables
1. INIT: resolve tables, MDL_SHARED_READ on them for the whole clone; merge any change buffer
   entries, or refuse when `innodb_change_buffering` is not none.
2. FILE_COPY of the selected .ibd only, page tracking on.
3. PAGE_COPY once: start redo archiving, send the tracked pages.
4. End point: MDL_SHARED_NO_WRITE on the tables (waits for open writers), stop redo archiving =
   end LSN, release. Nothing is sent under the lock.
5. REDO_COPY: send the archived redo up to the end LSN.

### Recipient (running server, no second mysqld)
1. Receive the files into a stage directory.
2. Redo apply on the staged files with the driver: records of the staged spaces only, applied to
   private blocks through `fil_tablespace_iterate`, by recovery's own per-record functions.
   - 8.4: driver around `recv_parse_log_rec` + a non-static wrapper of
     `recv_parse_or_apply_log_rec_body` with a context argument instead of `recv_sys`.
   - 26.07: the same driver on `ib::redo::Redo_applier`.
3. The files are now as of the end LSN with no uncommitted changes: no undo needed.
4. Rename into place and `IMPORT TABLE FROM '<file>.ibd'` (ps-8.4-import-refactor), which clears
   committed leftovers (trx ids, roll ptrs, delete marks).

### Still open (not blocking the design)
- Syntax (section 7). Protocol bump agreed (Decisions, 2026-10-18).
- Refusals for v1: ROW_FORMAT=COMPRESSED (unless the private block carries page_zip), general
  tablespaces, FULLTEXT needs a rebuild after import.
- Tests: driver vs crash recovery page comparison; MTR clone under concurrent DML.

## 24. Locking many tables at the end (CLONE DATABASE)

- Use the same method as `FLUSH TABLES t1, t2, ... FOR EXPORT` and `LOCK TABLES`: build one MDL request
  list and call `MDL_context::acquire_locks`. It sorts the requests by MDL key
  (`filter_and_sort_requests_by_mdl_key`) before acquiring them, so lockers that use this path can't
  deadlock with each other.
- A user transaction that already writes t1 and then wants t2, while we hold t2 and wait for t1, forms a
  cycle. MDL's deadlock detector (`find_deadlock`) breaks it by aborting the lower-weight side, which is
  the user's DML (ER_LOCK_DEADLOCK). That is the same behaviour as FOR EXPORT today.
- The real cost: while we wait for t1's open writer, the SNW locks already granted block writes on the
  other tables. Bound the wait with a short `lock_wait_timeout`. On timeout, release everything and retry
  with backoff. Nothing has been sent under the lock, so a retry is free; after N tries, fail the clone.
- One end LSN for all tables, so FKs between them are consistent.
- The table list is fixed at INIT (MDL_SHARED_READ on each table for the whole clone). A table created in
  the database during the clone is not included.

## 25. CLONE DATABASE: one consistent view of all tables

DDL is blocked for the whole clone by the backup lock (`LOCK INSTANCE FOR BACKUP`; clone takes it by
default). The table list is fixed at INIT and cannot change.

Consistency across tables comes from **one end LSN**, not from copying the files at the same moment.
This is the same reason a full clone is consistent:

```
t1.ibd copied at LSN 1000 (fuzzy)      redo 900..5000 applied  ->  t1 as of LSN 5000
t2.ibd copied at LSN 3000 (fuzzy)      redo 900..5000 applied  ->  t2 as of LSN 5000
                                        (redo archive started before the copy)
```

Each file is copied at a different time, but redo apply moves every file to the same LSN. A transaction
that changed t1 and t2 is either fully before the end LSN in both files or in neither, because commit
order is redo order.

The only thing the redo cannot fix is a transaction still **open** at the end LSN (a full clone rolls it
back with undo). So the end point is:
1. one `acquire_locks` call with SNW on every table of the database (sorted, so no deadlock between
   lockers; DDL cannot take part because the backup lock already excludes it);
2. it returns once every open writer on those tables has committed or rolled back;
3. stop the redo archive: that LSN is the end LSN;
4. release.

A user transaction that holds a write on t1 and then waits for t2 while we hold t2 is the only possible
cycle. The MDL deadlock detector aborts the user statement. A short `lock_wait_timeout` with retries
bounds how long writes on the already-locked tables wait (section 24).

## 26. How others get a consistent view of one database

| tool | how | locks | undo |
|---|---|---|---|
| mysqldump --single-transaction, mydumper, MySQL Shell dump | MVCC read view (REPEATABLE READ snapshot) on the donor; logical rows | none for InnoDB (Shell/mydumper: backup lock, or a brief FTWRL to start all threads on one snapshot) | the donor's live undo builds old versions |
| PXB --databases / --tables | physical copy + all redo; `--prepare` = full crash recovery incl. rollback, then `--export` | backup lock only (DDL) | yes: ibdata + undo shipped, rollback at prepare |
| FLUSH TABLES ... FOR EXPORT | quiesce | SNW on every table | no |

Every lock-free method relies on undo: the donor's live undo (MVCC) or shipped undo plus rollback (PXB).
The table-lock drain (sections 18, 24, 25) is the FOR EXPORT model and has its problems on many tables.

### Candidate: corrections from the donor's MVCC, no table locks (not verified)
1. End point: stop the redo archive (end LSN). Under `trx_sys` at the same moment, record the set S of
   rw transactions active at that LSN, and open a read view RV.
2. Donor: for each trx in S, read its undo records for our tables to get the PKs it touched. For each
   PK, read the row through RV (normal consistent read) and send `(PK, committed row | absent)`.
3. Recipient: after redo apply, apply the corrections (put back the committed row, or delete the row),
   then IMPORT. The result is the committed state as of the end LSN.
- Uses existing mechanisms only: read view, undo record parsing, PK lookup. One direction.
- Open: insert undo is freed when the trx commits, so PKs of inserts must be read before that (or a trx
  in S must not finish before its PKs are collected). A long-running trx with many changes means many
  corrections.

## 27. Physical + undo on a live recipient (to study)

What is needed: roll back, on the recipient, the transactions that were open at the end LSN, for the
selected tables only. The nearest existing mechanism is the **background rollback of recovered
transactions at startup**: `trx_resurrect` builds trx objects from the undo segment headers, then
`trx_rollback_or_clean_recovered` runs `row_undo` while the server is already serving queries.

Sketch (unverified):
1. Donor: also ship the undo tablespaces (clone already copies them in a full clone), with redo for them.
2. Recipient: redo apply on the staged .ibd files and on the staged undo files.
3. Resurrect only the trx that were active at the end LSN and touched the selected tables, reading their
   undo from the staged undo files.
4. Roll them back against the staged tables, then IMPORT (which clears trx ids and roll ptrs).

Problems to study in code:
- The undo records name the donor's table ids, and the roll ptrs point into the donor's undo space ids.
  Both must be mapped onto the recipient.
- `row_undo` works through the buffer pool on attached tables, not on detached files. The tables may need
  to be attached under a hidden name first.
- How much of `trx_resurrect` assumes the undo spaces are the server's own (rseg array, trx_sys).

### 27a. What the rollback needs (checked in code)
- **mysql.ibd: no.** `row_undo_ins_parse_undo_rec` and `row_undo_mod_parse_undo_rec` read a table_id
  from the undo record and call `dd_table_open_on_id` on it. On the recipient that lookup must be a
  donor-id → staged-table map instead (the staged tables come from their SDI). The donor's DD is not used.
- **ibdata: no.** In 8.0 user transactions use rsegs in the undo tablespaces; each undo space lists them
  on its RSEG_ARRAY page (`FSP_RSEG_ARRAY_PAGE_NO` = 3). `trx_resurrect(rseg)` starts from the rseg.
- **Picking the transactions**: `trx_resurrect_table_ids` already collects the table ids a resurrected
  trx touched. Keep only trx that touched a selected table.

### 27b. Whole undo tablespaces, or only some pages?
- The set of transactions open at the end LSN is known only at the end, so the copy cannot be filtered
  at FILE_COPY time.
- Copying the undo pages of those transactions at the end, without redo, races with the donor: a trx that
  commits frees (or caches for reuse) its insert undo, and purge frees update undo.
- **So: copy the whole undo tablespaces**, as a full clone does, and apply redo to them up to the end LSN.
  The undo pages are then exactly as of the end LSN, with no race. The cost is the undo size, which is
  large when the history list is long.
- Possible later reduction: skip pages that are free in the undo space's extent descriptors.

## 28. Undo without trx_resurrect: an undo driver, symmetric to the redo driver (proposal)

Donor: **no new mechanism**. Undo tablespaces go through FILE_COPY / PAGE_COPY / REDO_COPY exactly as in a
full clone; the filter applies to user tablespaces only. End LSN = stop the redo archive, as today. No
MDL drain, no table locks at all; the backup lock (DDL) stays for the whole clone.

Recipient, per staged table:
1. **Redo apply** (driver, section 20) on the staged .ibd and undo files → all as of the end LSN. The same
   page pass also rewrites the page header fields import needs before anything touches the tree: space
   id, index id (recipient's), page LSN.
2. **Attach raw**: create the DD entry from the SDI and attach the file (the import-refactor attach step,
   `row_import_convert` split out), **without** the row cleanup. Trx ids, roll ptrs and delete marks stay.
   The table is invisible/X-locked until step 4.
3. **Undo driver**: read the staged undo files with private-block I/O: RSEG_ARRAY page → rseg headers →
   undo segments in state ACTIVE or PREPARED at the end LSN. For each such trx, walk its records in
   reverse undo_no order (what `trx_roll_pop_top_rec_of_trx` does through the buffer pool), keep records
   whose table id maps to a staged table, and call the existing `row_undo_ins_*` / `row_undo_mod_*`
   functions on an `undo_node_t` with a background trx. Needed hook: `row_undo_ins_parse_undo_rec` and
   `row_undo_mod_parse_undo_rec` take `node->table` from a donor-id → table map when one is set, instead
   of `dd_table_open_on_id`. `row_undo_search_clust_to_pcur` matches the row by PK and checks
   `roll_ptr == node->roll_ptr` (row0undo.cc), which holds because step 2 kept the roll ptrs.
   Rollback writes no undo (BTR_NO_UNDO_LOG_FLAG) and goes through the recipient's redo; a crash mid-way
   is covered by import's DDL-log cleanup (drop the half-imported table, retry the clone).
4. **Row cleanup** (import's existing PageConverter record pass: reset trx id / roll ptr, drop
   delete-marked rows) and make the table visible. After step 3 every remaining delete mark is committed.

Why not section 27's trx_resurrect: it needs the staged undo spaces registered as the server's rsegs and
trx objects in trx_sys; the driver needs neither. Pages passes: redo apply + cleanup = 2, same as before.

### Known flaws / costs
- **Undo volume.** Whole undo tablespaces + their redo for the clone's duration, even for one small
  table. A long history list means GBs. Later: skip pages free in the undo space's extent descriptors.
  (Shipping only the open trx's undo pages at the end races with commit cleanup and purge, see 27b. A
  donor-side pause under each open trx's `undo_mutex` would avoid the race, but it is a new mechanism:
  park it.)
- **Semantics.** XA PREPARED trx are rolled back on the copy (snapshot semantics, like mysqldump).
- **Not in v1.** FULLTEXT (aux tables, cache; rebuild after import), general tablespaces, compressed
  columns, change buffering ≠ none. Partitions: one tablespace each, same driver, more files.
- **To check in code.** Undo tablespace truncation on the donor during the clone (does the truncate wait
  for an active clone, or does clone restart the copy?). Index-id rewrite before rollback: debug asserts
  compare `btr_page_get_index_id(page)` with `index->id`.
- **Two code lines.** The redo driver differs on 8.4 / 26.07; the undo driver uses `row_undo_*`, which is
  stable across both.

### 28a. Which undo tablespaces to copy (checked in code)
- A trx gets its rseg at its first write by round robin over **all** undo tablespaces
  (`get_next_redo_rseg_from_undo_spaces`, trx0trx.cc: "(0,0), (1,0), ... (n,0), (0,1), ..."), including
  inactive ones that still have rsegs. The set S of trx open at the end LSN is known only at the end, and
  the base copy must be taken before the end LSN. So we cannot pick spaces: **copy every undo tablespace**,
  as a full clone does.
- Size is bounded in practice: `innodb_max_undo_log_size` defaults to 1 GiB per space, and with
  `innodb_undo_log_truncate=ON` (default in 8.0) a space larger than that is truncated once its rsegs
  are empty. Default 2 spaces → at most ~2 GiB of undo plus their redo, independent of the table size.
- Undo truncate during the clone: `trx_purge_truncate_marked_undo` takes the backup-lock MDL and
  re-checks `clone_check_provisioning()`, so it waits for the clone (trx0purge.cc). Checked.
- Reduction that is purely physical, for later: skip pages free in the XDES bitmap at FILE_COPY. Safe
  because every page allocation logs MLOG_INIT_FILE_PAGE2 (`fsp_init_file_page`, fsp0fsp.cc), so a page
  allocated after the copy rebuilds from zeros, as section 20 already assumes for new user pages. A page
  allocated between the XDES read and the copy is tracked at flush and resent. The history-list part of
  the undo (committed, awaiting purge) cannot be told apart at copy time without segment-state logic,
  which is a new mechanism: park it.

## 29. Ready to implement? What is settled, what is not

### Settled (design)
Donor: filtered FILE_COPY (selected .ibd + all undo), PAGE_COPY, REDO_COPY to the end LSN, backup lock
held, new protocol version carrying the table list. Recipient: stage → redo driver (also rewrites space
id / index id / LSN) → attach raw → undo driver → import cleanup → visible. No table locks, no undo on the
wire beyond the undo tablespaces, no second mysqld.

### Must verify in code before coding (half a day)
1. Debug asserts on `btr_page_get_index_id(page) == index->id` in btr/page code → confirms index ids
   must be rewritten in the redo pass, before rollback.
2. `undo_node_t` + `que_thr_t` setup for `row_undo_ins_*` / `row_undo_mod_*` outside `trx_rollback`
   (what `node->trx`, `thr`, `node->state` must be; which fields of the dummy trx are read).
3. The two `*_parse_undo_rec` functions: a table map hook instead of `dd_table_open_on_id` (and that
   purge, which uses the same functions, is unaffected).
4. Encrypted undo tablespaces: clone already re-wraps their keys; confirm the private-block reader can
   decrypt them (same path as encrypted user files in fil_tablespace_iterate).
5. Which `validate_remote_params` checks remain for a partial clone (page size, version compat;
   not plugins/charsets of the whole instance).
6. The attach-raw split in the import branch (`row_import_convert` boundary) is where step 2 of
   section 28 plugs in; coordinate with the ps-8.4-import-refactor owner.

### Decisions (taken 2026-10-18, see Decisions log)
- **Line first**: 8.4 (import branch is there; redo driver hand-written) or 26.07 (Redo_applier; import
  branch must be ported). Both eventually.
- **Syntax**: confirm section 7 (`CLONE TABLE db.t[, ...] FROM ...`, `CLONE DATABASE db FROM ...`).
- **Protocol version value**: 0x0104 to stay above trunk's 0x0103, or coordinate with upstream.
- **Objects other than tables** in CLONE DATABASE: v1 = tables only (SDI has no triggers, views,
  routines, events); documented, or refuse when the schema has them?
- **FKs to tables outside the set**: fail, or import with the FK unchecked (FOREIGN_KEY_CHECKS=0 style)?
- **Name clash on the recipient**: fail (proposed), no overwrite.
- **Scope of v1 refusals**: COMPRESSED row format, general tablespaces, compressed columns, FULLTEXT
  (or FULLTEXT allowed + automatic rebuild), change buffering ≠ none, partitions (allowed, one file each).

### Implementation order (each step testable alone)
1. Donor filter + locator/protocol + `CLONE TABLE` syntax, recipient staging only (files land in
   `#clone_stage/`); test: files match a FOR EXPORT copy taken under no load.
2. Redo driver on staged files; test: pages equal to crash recovery (section 20 tests).
3. Attach raw + import cleanup split; test: import under no load end to end.
4. Undo driver; test: clone under concurrent DML with open trx at the end, CHECK TABLE + checksums
   against the donor's committed state at the end LSN.
5. `CLONE DATABASE`, error paths (crash mid-import, network retry, refusals), MTR suite.
