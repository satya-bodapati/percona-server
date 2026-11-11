# MyRocks in Group Replication and PXC: code walk

Companion to `DESIGN-NOTES-myrocks-gr-pxc.md`. Code reading only, 2026-11-11; nothing was built or
run. Items marked (unverified) were inferred, not observed.

## W1. Percona Server 8.4: Group Replication and RocksDB

### W1.1 Engine check and the PS-6820 option

- Commits: 857fd38793d (PS-6820), merge ba6efe693ae.
- `plugin/group_replication/CMakeLists.txt:167-173`: `IF(GROUP_REPLICATION_WITH_ROCKSDB)`, no
  `OPTION()`, off by default. The line
  `ADD_COMPILE_DEFINITIONS(src/observer_trans.cc ... COMPILE_DEFINITIONS "GROUP_REPLICATION_WITH_ROCKSDB")`
  passes file names as definitions, so `src` becomes a macro and
  `include/certification/certified_gtid.h:64` would not parse (unverified: not built; the flag
  alone fails under `g++ -Werror`).
- The option only extends `db_type != DB_TYPE_INNODB` to accept `DB_TYPE_ROCKSDB`:
  `plugin/group_replication/src/observer_trans.cc:140-148` (local transactions) and
  `src/asynchronous_channels_state_observer.cc:239-247` (async channels into a member).
- Error: `ER_GRP_RPL_NEEDS_INNODB_TABLE` (`share/messages_to_error_log.txt:4683-4684`), surfaced as
  `ER_BEFORE_DML_VALIDATION_ERROR` (`sql/sql_base.cc:5396-5398`).
- Engine type source: `sql/rpl_handler.cc:698`; `DB_TYPE_ROCKSDB = 42` (`sql/handler.h:689`,
  `storage/rocksdb/ha_rocksdb.cc:8429`).
- No other InnoDB test in the plugin.
- Primary key required: `observer_trans.cc:150-154`; key count from `sql/rpl_handler.cc:685-696`.
  Hidden-PK RocksDB tables have `primary_key == MAX_INDEXES` (`rdb_datadic.cc:2298-2300`).
- SERIALIZABLE refused only with update-everywhere checks (`observer_trans.cc:127-131`).

### W1.2 Write sets

- `add_pke()`, `sql/rpl_write_set_handler.cc:761-1090`: walks `table->key_info` for `HA_NOSAME`
  keys (`:822-825`), hashes with collation `strnxfrm` (`:864-865`, `:893`). No engine encoding.
- Foreign-key hashes from `TABLE_SHARE::foreign_key` (`:973-1035`); RocksDB refuses foreign keys
  (`ha_rocksdb.cc:11246-11250`).
- No key hash sets `set_has_missing_keys()` (`:1089`).

### W1.3 High priority and lock waits

- GR applier priority: `replication_threads_api.cc:68`, `replication_threads_api.h:34`, applied in
  `sql/rpl_replica.cc:345-350`.
- InnoDB: `thd_trx_priority()` (`ha_innodb.cc:1998`), `trx_kill_blocking()`
  (`trx0trx.cc:3544-3554`), hit list (`lock0lock.cc:1977`), callers `row0ins.cc:1774`,
  `row0mysql.cc:1176`, `row0sel.cc:5918`; queue jump (`lock0lock.cc:571-574`, `2184`).
- RocksDB: no read of the priority, no `kill_connection` hook (`ha_rocksdb.cc:8412-8447`). Waits end
  at `rocksdb_lock_wait_timeout`, default 1 s (`:1245-1249`, `:3725-3728`); deadlock detection off
  by default (`:1259-1260`).
- Commit-order deadlocks: GR requires `replica_preserve_commit_order` (`plugin.cc:2979-2982`);
  detection needs `thd_report_lock_wait` (`sql/sql_thd_api.cc:694-707`), called only by InnoDB
  (`lock0lock.cc:1414`). PS-11845.
- Failover window: `plugin/group_replication/src/plugin_handlers/primary_election_primary_process.cc`
  waits for buffered transactions before leaving read mode (`:161-180`), then waits for old-primary
  transactions with writes open (`:299-304`).

### W1.4 XA and two-phase commit

- Handlerton hooks set at `ha_rocksdb.cc:8415-8424`. PS-11631 = 7ce0054f5b4, PS-11632 = 9f1b378ac66
  (`XA_PREPARED` marker, `:6458-6500`).
- XA PREPARE refused on a write-batch transaction (`:6435-6443`).
- No `rocksdb_enable_2pc`; WAL sync by `rocksdb_flush_log_at_trx_commit` in `rocksdb_flush_wal`
  (`:6389-6409`).
- RocksDB stores no binlog position; the `slave_gtid_info` comment (`:6422-6423`) is stale.

### W1.5 Gap locks, isolation, savepoints

- `rocksdb_gap_lock_raise_error` (`:1251-1257`, `:6160-6173`);
  `handler::is_using_prohibited_gap_locks` (`sql/handler.cc:8717-8733`) skips `thd->rli_slave`.
- Locking reads under binlog above READ COMMITTED (`:14421-14431`); snapshots at REPEATABLE READ
  (`:7964-7972`).
- `savepoint_set` is a dummy (`:7979-7983`); rollback calls `rollback_to_savepoint` (`:7985-7989`).

### W1.6 DDL

- Flags: `HTON_SUPPORTS_EXTENDED_KEYS | HTON_CAN_RECREATE | HTON_SUPPORTS_ONLINE_BACKUPS`
  (`:8447-8448`); `HTON_SUPPORTS_ATOMIC_DDL` is at `sql/handler.h:3127`.
- Own dictionary `Rdb_ddl_manager` (`:204`); `rocksdb_validate_tables` (`:2481-2487`,
  `rdb_datadic.cc:5117-5130`).
- CREATE ... SELECT under GTID: `sql/binlog.cc:11185-11197`.

### W1.7 Replica options

- `rocksdb_rpl_skip_tx_api`: write batches, no locks (`:6236-6241`).
- `rocksdb_read_free_rpl` (`:1459-1466`), `use_read_free_rpl` (`:19225-19240`), `skip_unique_check`
  (`:12550-12563`).

### W1.8 Mixed engines

- No refusal. Tests: `rocksdb_rpl/t/rpl_rocksdb_row_consistent_shapshot_mixed_engines.test`,
  `rpl_rocksdb_stm_mixed_consistent_shapshot_mixed_engines.test`.

### W1.9 Clone on 8.4

- No `clone_interface` in the RocksDB handlerton (`:8412-8448`).
- `plugin/clone/src/clone_hton.cc:107` iterates engines; `:70` skips those without `clone_begin`.
- Recipient: `validate_schemas` reports the missing table (`rdb_datadic.cc:4801-4808`); init fails
  with `validate_tables=1` (`rdb_datadic.cc:5124-5129`, `ha_rocksdb.cc:8883-8892`) (unverified).
- `mysql-test/suite/rocksdb_clone/` has one test, `compression_config.test` (a165f521ea3).
- Building blocks: `rocksdb_create_checkpoint` (`:2290-2293`),
  `rocksdb_create_temporary_checkpoint` (`:2295`, `:6330`), `rocksdb_disable_file_deletions`
  (`:2312-2315`), log_status hooks (`:8431-8433`, `:7853-7914`).
- GR clone choice: `remote_clone_handler.cc:271-300`, donor info `:153-175`.

### W1.10 Tests

- No rocksdb mention in `mysql-test/suite/group_replication` or `mysql-test/suite/clone`.
- `rocksdb_rpl`: `rpl_rocksdb_2pc_crash_recover`, `rpl_rocksdb_row_crash_safe`,
  `rpl_rocksdb_stm_mixed_crash_safe`, `multiclient_2pc`, `rpl_rocksdb_xa_detach`,
  `rpl_xa_skip_tx_api`, `read_free_repl_idempotent_mode`, `rocksdb_skip_locks_if_skip_unique_check`,
  `rpl_rocksdb_snapshot_without_gtid`.

## W2. PS-11475 branch (`marcinbabij/percona-server:PS-11475`)

Base dea339f95fc, 14 commits, last 2026-11-10, 51 files, +2544/-75. No PR.

- 4b3d0ccd0be: Yves Trudeau's patch (`myrocks_clone.diff` on the ticket). Adds
  `-DGROUP_REPLICATION_WITH_ROCKSDB=1` to `build-ps/build-binary.sh`; fixes the CMake line (W1.1)
  with `MY_ADD_COMPILE_DEFINITIONS`.
- 44f6f1e92fc: basic provisioning test. Then ten review fixes "#1".."#10".
- Callbacks in `storage/rocksdb/rdb_clone.{h,cc}`: capability (`HA_CLONE_BLOCKING`,
  `HA_CLONE_MULTI_TASK`), begin, copy, ack (no-op), end, apply_begin, apply, apply_end. RESTART
  refused.
- Donor: `clone_begin` runs `rocksdb::Checkpoint::CreateCheckpoint` into `.rocksdb_clone_<session>`;
  the master task streams files in chunks of at most 1 MiB.
- Consistency: `plugin/clone/src/clone_hton.cc` `Clone_read_lock` takes the global read lock and
  blocks commits when an engine is BLOCKING. `sql/clone_handler.cc` `XA_Block` skips XA counting
  when the thread holds the commit lock.
- Recipient: staging directory, `CLONE_COMPLETE` marker after fsync; `rdb_clone_apply_pending()` at
  startup installs by rename, `rdb_clone_finish_pending()` removes the backup.
- Tests: `mysql-test/suite/rocksdb_clone/t/ps11475_*` (provision default/cf, local default/custom,
  workers, remote_large, write_barrier, xa_barrier, failed_begin, begin_cleanup); gunit
  `ps11475_clone-t.cc`, `ps11475_hton-t.cc`.
- Note file `PS-11475-myrocks-issues.bd`: "Failed to prepare 1 XA transaction(s)" after cloning a
  prepared XA.

## W3. PXC 8.4 (74654e7c688; galera 13ff9ed, wsrep-lib 96ab92c)

### W3.1 Build and refusals

- `storage/rocksdb/CMakeLists.txt:1` `RETURN()`; `MYROCKS_VERSION` 9.3.1-5.
- `build-ps/debian/rules:112,168,246`, `build-ps/pxc_builder.sh:216-221,885,887`,
  `build-ps/build-binary.sh:503,552,599`.
- `sql/sql_base.cc:6253-6300` (DML), `sql/sql_alter.cc:490-556`, `sql/sql_truncate.cc:829`,
  `sql/sql_admin.cc:1908,1977`, `sql/sql_cmd_ddl_table.cc:249`.
- MyISAM by TOI: `sql/sys_vars.cc:8508`, `sql/sql_base.cc:6363-6372`.
- Not replicated without keys: `sql/wsrep_trans_observer.h:202-215`; wsrep-lib
  `include/wsrep/transaction.hpp:125-128`; pxc-docs `docs/limitation.md:5-7`.

### W3.2 Engine hooks

- Handlerton: `sql/handler.h:1752-1758`, `2889-2893`; `HTON_WSREP_REPLICATION` `:3217`; virtual
  `wsrep_append_keys` `:7550`.
- InnoDB registration: `ha_innodb.cc:5943`, `5998-6000`.
- Write set data is the binlog cache: `sql/wsrep_binlog.cc:169-214`, `sql/binlog.cc:12549`.
- Prepare hooks inside the engine loop: `sql/handler.cc:2568-2626`; savepoint hooks `2508-2530`.
- InnoDB keys: `ha_innodb.cc:13124-13290` (row hash without PK at `13262-13283`), helpers at `8780`,
  `8676`, `13066`; calls at `10638`, `11482`, `11573`; gate at `10354`.
- Foreign keys: `ha_innodb.cc:12931`, `row0ins.cc:71,1369,1719`; `sql/wsrep_mysqld.cc:1728`, `1934`.

### W3.3 Brute-force abort

- `ha_wsrep_abort_transaction`, `sql/handler.cc:9000-9016`, InnoDB only (comment at `8989`).
- `ha_innodb.cc:24482-24528`, `24395-24480`; `sql/service_wsrep.cc:164-205`.
- Lock manager: `lock0lock.cc:926-960`, `2239-2243`, `1674`, `1900`, `2135`, `3567`;
  `lock0wait.cc:1023-1044`.
- RocksDB side: waits are killable (`storage/rocksdb/rdb_mutex_wrapper.cc:67-117`, PS). Holder ids
  are known at the conflict: percona/rocksdb@afc7fe1
  `utilities/transactions/lock/point/point_lock_manager.cc:252-343`; deadlock detection `375-417`.

### W3.4 Replay, TOI, NBO

- `sql/wsrep_client_service.cc:276`, `354`; `sql/wsrep_applier.cc:140`;
  `sql/wsrep_high_priority_service.cc:1095`; `sql/wsrep_mysqld.cc:2295-2350`, `2676`, `2852`;
  `trx0sys.cc:415-481`.

### W3.5 Checkpoint

- `trx0sys.h:340-349`; `trx0trx.cc:1633`, `1747-1794`; `ha_innodb.cc:22309`, `24531-24552`;
  `trx0sys.cc:500-524`.
- `sql/wsrep_xid.cc:107-175` (InnoDB only); readers `sql/wsrep_mysqld.cc:1350-1362`,
  `sql/wsrep_server_service.cc:294,328`.
- Reserved, unused slot in PS MyRocks: `BINLOG_INFO_INDEX_NUMBER`,
  `storage/rocksdb/rdb_datadic.h:1595-1597`.

### W3.6 SST and IST

- `sql/wsrep_sst.cc:73-89`, `126-144`, `1134-1150`; `scripts/wsrep_sst_xtrabackup-v2.sh:194`;
  `scripts/wsrep_sst_clone.sh`; `scripts/wsrep_sst_rsync.sh:219-229`.
- PXB 8.4: `backup_mysql.cc:348-350,523-525,1310-1330`; `backup_copy.cc:1227-1330,1433-1550`;
  `src/wsrep.cc:153-171`; `test/suites/rocksdb/` including `cross_engine_trx.sh`.
- IST apply: `sql/wsrep_high_priority_service.cc:671`.

### W3.7 Tests

- No rocksdb mention in any galera or wsrep suite.

### W3.8 MariaDB

- `storage/rocksdb/ha_rocksdb.cc` has no wsrep code; no `HTON_WSREP_REPLICATION` (main `:5345-5346`).
- `sql/handler.cc:8558-8566` (`wsrep_check_if_binlog_row`); `sql/wsrep_mysqld.cc:2590-2630`,
  `1479-1550`; `mysql-test/suite/galera/t/MDEV-35748.test`.
- Docs: https://mariadb.com/docs/server-1/server-usage/storage-engines/myrocks/about-myrocks-for-mariadb
  and https://mariadb.com/docs/galera-cluster/reference/mariadb-galera-cluster-known-limitations

### W3.9 Not verified for PXC

Galera certification of write sets with no keys; snapshot conflicts inside appliers; the MDL abort
fallback for a RocksDB-only victim; recovery after a crash between the two engine commits; NBO with
MyRocks DDL.

## W4. Tickets

- PS-11475: In Progress, Marcin Babij, reporter Yves Trudeau, blocks K8SPS-861.
- PS-7782: epic, On Hold since 2025-02-07, no reason recorded; only child PS-7783 (Open, Satya).
- PS-6820: Done. PS-6068: Won't Do. PS-6073: Done (docs). PS-4462: Won't Do, not clone.
- PS-11628 children. Pending release: PS-11631, PS-11632, PS-11903, PS-11881. Open: PS-11688,
  PS-11773, PS-11761, PS-11898, PS-11845, PS-11912, PS-11832, PS-11679, PS-11795, PS-11844.
- K8SPS-860, K8SPS-861: operator must block GR + MyRocks.
- PXB-3355, PXB-3083, PXB-3288, PXB-1754 / PS-5360: MyRocks backup in xtrabackup.
- PXC-4149: PXC build with `WITH_ROCKSDB=ON` failed; disabled in CI.
