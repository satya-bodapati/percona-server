# MyRocks in Group Replication and PXC: brief

2026-11-11. Code reading only; nothing was built or run. Long code walks with all file:line
references are in `DESIGN-NOTES-myrocks-gr-pxc-codewalk.md` (sections W1-W4, cited below).
Paths are Percona Server 8.4 (this worktree) unless marked PXC (percona-xtradb-cluster 8.4 at
74654e7c688) or PXB (percona-xtrabackup 8.4).

Decided by Satya: Group Replication (GR) supports MyRocks in single-primary mode only.

## Summary

- GR single-primary is reachable: the plugin has one engine check, write sets are engine-agnostic,
  and the remaining gaps are clone, applier lock waits and DDL crash safety.
- PXC is a much larger job: MyRocks is not built there, every wsrep engine hook is InnoDB-only, and
  neither MariaDB nor anyone else has Galera + MyRocks code to reuse.
- Clone is the shared first step, and PS-11475 is not yet what GR needs: it blocks the donor for the
  whole clone.

## 1. Clone

Today on 8.4: RocksDB has no clone interface, so clone skips it silently. The recipient gets the
dictionary without the RocksDB data and fails startup validation (W1.9; PS-6068, closed Won't Do).

Corrections to the ticket list: PS-4462 is Won't Do and is about consistent-snapshot transactions,
not clone; it gives no code. PS-7783 (clone API for MyRocks) is Open, assigned to Satya, empty.

PS-11475 (Marcin, branch `marcinbabij/percona-server:PS-11475`, 14 commits, no PR; W2):

- All eight clone callbacks, in `storage/rocksdb/rdb_clone.{h,cc}`.
- Snapshot = RocksDB checkpoint (hard links), streamed by one task.
- Consistency with InnoDB, binlog and GTID = the donor takes a global read lock and blocks commits for
  the whole clone (`plugin/clone/src/clone_hton.cc`, `Clone_read_lock`). No log_status, no locator.
- No restart: a clone that loses its connection starts over.
- 10 MTR tests in `rocksdb_clone`, none with GR.
- Also sets `-DGROUP_REPLICATION_WITH_ROCKSDB=1` in `build-ps/build-binary.sh`.
- Julia Vural's comment (2026-11-09) points to a second solution from Marco Tusa (Docker image);
  unreviewed.

What GR distributed recovery needs on top:

| Need | State |
|---|---|
| InnoDB + RocksDB + GTID at one point | Met, by blocking commits |
| Donor stays writable | Not met. The donor is a live member; if it is the primary, the group's writes stop for the clone |
| Dictionary | Met: the DD is InnoDB, cloned as usual |
| Resume after network loss | Not met |
| GR chooses clone by GTID only, no engine check (`remote_clone_handler.cc:271-300`) | Works once clone carries RocksDB |

A non-blocking version needs a RocksDB end point tied to InnoDB's: the building blocks exist
(`LockWAL` and the WAL file list in `performance_schema.log_status`, `ha_rocksdb.cc:7853-7914`; PXB
uses exactly these, PXB `backup_copy.cc:1227-1550`).

### 1a. Meta's MyRocks clone (fb-mysql-8.0.32, commit 5d31d0a9f0, PR facebook/mysql-5.6#1250)

Laurynas Biveinis, 2023; 216 files; `storage/rocksdb/clone/{common,donor,client}.{h,cc}`. None of
it is in PS 8.4 (no `clone_precopy`, `set_log_stop` or `storage/rocksdb/clone`).

- Donor stays writable. Engines are synchronised by a `performance_schema.log_status` query run
  from the clone plugin (`synchronize_engines`), not by a read lock.
- Two new handlerton calls: `clone_precopy` (copy before the consistent point) and `set_log_stop`
  (gives each engine its stop position).
- RocksDB donor: states NONE, ROLLING, FINAL, FINAL_WITH_LOGS. SST files are copied in precopy from
  rolling checkpoints; at `set_log_stop` the WAL files are added.
- InnoDB changes: redo archiving stops at the log_status LSN, also in the middle of a block; crash
  recovery of the clone keeps recovered dynamic metadata. New InnoDB clone state "SST COPY".
- Capabilities: HYBRID, MULTI_TASK, RESTART (resume after a network error).
- Clone plugin fixes: O_DIRECT files of any size, size estimates from several engines, rollback
  marker shared between engines, error ACK always sent to InnoDB too.
- Tests: `rocksdb_clone` suite reusing the InnoDB clone tests.
- Stated limits: concurrent DDL not supported; `clone.local_xa` disabled because the new
  synchronisation no longer blocks on XA; "this change breaks some of the clone-related group
  replication MTR tests ... this patch makes no effort to fix them".

### 1b. What Meta's clone changes in InnoDB clone (Satya's concern)

In `Clone_Snapshot::init_redo_copy` Meta disables, with `#if 0`, the block that upstream and PS 8.4
run before stopping redo archiving (`storage/innobase/clone/clone0copy.cc:474-510`):

- `Clone_handler::XA_Block` (external XA blocked during the switch).
- `synchronize_binlog_gtid` (`clone0copy.cc:332-348`): wait for binlog-prepared transactions, flush
  the GTID persister (`gtid_persistor.wait_flush`, "Persist non-innodb GTIDs"), write the binlog
  position to the trx sys page.
- `dict_persist_t::Enable_immediate` for dynamic metadata; replaced by a change in clone recovery.

In its place: `clone_cbk->synchronize_engines()` (log_status query), redo archiving that stops at
that LSN (`arch0log.cc`), and recovery changes (`log0recv.cc`, `srv0start.cc`).

Risk for PS (inferred, not tested): GTIDs of RocksDB-only transactions live only in memory and the
binlog until the persister flushes them to `mysql.gtid_executed`. Without the flush at the clone
point the recipient can hold RocksDB rows whose GTIDs it does not know, and GR recovery would fetch
those transactions again. In the patch only the storage-engine part of log_status is used. Meta's
MyRocks stores binlog position and GTID in RocksDB itself; PS MyRocks does not
(`BINLOG_INFO_INDEX_NUMBER` is reserved and unused, `rdb_datadic.h:1595-1597`).

A third shape, not implemented anywhere: copy SST files online from rolling checkpoints (Meta's
precopy), keep the upstream InnoDB block unchanged, and block commits only for the short final step
in which the RocksDB WAL files are added.

For PXC, clone SST exists (`scripts/wsrep_sst_clone.sh`) but the runtime check accepts only
`xtrabackup-v2` (PXC `sql/wsrep_sst.cc:126-144`).

## 2. Group Replication

| Area | Finding | Class | Size |
|---|---|---|---|
| Engine check | Two places, relaxed by `GROUP_REPLICATION_WITH_ROCKSDB` (`observer_trans.cc:140-148`, `asynchronous_channels_state_observer.cc:239-247`). The CMake line is wrong and probably does not compile (`CMakeLists.txt:167-173`); Marcin's branch fixes it | Blocker | S |
| Mode enforcement | Nothing refuses MyRocks in multi-primary or on `group_replication_switch_to_multi_primary_mode()` | Blocker (new) | S-M |
| Write sets | Engine-agnostic, from `table->key_info` (`sql/rpl_write_set_handler.cc:761-1090`) | Works | - |
| Primary keys | Hidden-PK RocksDB tables are refused (`observer_trans.cc:150-154`, `rdb_datadic.cc:2298-2300`) | Limitation | - |
| Foreign keys | RocksDB has none (`ha_rocksdb.cc:11246-11250`) | Works | - |
| Remote kills local | RocksDB never reads the transaction priority and has no kill hook (W1.3). Single-primary removes the main case | Limitation | - |
| Applier lock waits | Still reachable in single-primary: locking reads on secondaries; the failover window (`primary_election_primary_process.cc:161-180`, `:299-304`); parallel workers deadlocking on commit order because RocksDB never calls `thd_report_lock_wait` (PS-11845, open). Exit is only `rocksdb_lock_wait_timeout` (1 s) plus retries | Blocker or limitation: open question | M |
| Gap locks, isolation | Applier is exempt (`sql/handler.cc:8717-8733`); local REPEATABLE READ errors are existing MyRocks behaviour | Limitation | - |
| XA, 2PC | Hooks present after PS-11631/11632. Open: PS-11688, PS-11773, PS-11761, PS-11898. Every applier commit is InnoDB + RocksDB 2PC, because applier metadata and `gtid_executed` are InnoDB | Blocker until fixed | M |
| DDL | Not atomic (no `HTON_SUPPORTS_ATOMIC_DDL`, `ha_rocksdb.cc:8447-8448`); a crashed DROP diverges a replica (PS-11832, open). CREATE ... SELECT is refused under GTID | Limitation, PS-11832 a blocker | M-L |
| Savepoints | SQL-level write-set savepoints are engine-independent; engine `savepoint_set` is a dummy (`ha_rocksdb.cc:7979-7989`) | Works, needs a test | S |
| Read-free replication, `rpl_skip_tx_api` | Would apply to the GR applier. `rpl_skip_tx_api` loses transactions on crash (PS-11912) | Refuse or document | S |
| Mixed InnoDB + RocksDB | No refusal; binlog coordinates | Works, needs tests | S |

Reuse: MySQL upstream has nothing (GR is InnoDB-only there). Meta runs MyRocks with Raft, not GR.
fb-mysql has a non-blocking MyRocks clone; see 1a.

## 3. PXC

MyRocks is not built in PXC: `storage/rocksdb/CMakeLists.txt` starts with `RETURN()`, packaging sets
`WITH_ROCKSDB=0`, and the copy is stale (9.3.1-5 vs 9.7.4-1). CI disabled it under PXC-4149. No PXC
ticket covers MyRocks. Details in W3.

| Area | InnoDB-only today | RocksDB would need | Class | Size |
|---|---|---|---|---|
| Build | Disabled | Enable, refresh the engine, packaging | Blocker | M |
| `pxc_strict_mode` | Every check is `!= DB_TYPE_INNODB` (`sql/sql_base.cc:6253-6300`, `sql_alter.cc:490-556`, others) | Allow RocksDB in each | Blocker | S |
| Certification keys | `ha_innobase::wsrep_append_keys` (`ha_innodb.cc:13124-13290`). An engine that appends no keys commits locally and is never replicated | `ha_rocksdb::wsrep_append_keys` for PK and unique keys, called from write/update/delete | Blocker | M |
| Prepare hooks | Run inside the per-engine loop (`sql/handler.cc:2568-2626`) | Once-per-transaction guard for two flagged engines | Blocker | S-M |
| Brute-force abort | `ha_wsrep_abort_transaction` looks up InnoDB only (`handler.cc:9000-9016`); lock manager hooks in `lock0lock.cc` | Holder-side abort from RocksDB's point lock manager, never-victim rule for appliers, dispatcher over all engines | Blocker, hardest item | L |
| Replay, TOI, NBO | Server-level | NBO with MyRocks DDL unverified | Mostly works | S |
| Checkpoint | wsrep XID in the InnoDB system header, same mini-transaction as commit (`trx0trx.cc:1747-1794`); `sql/wsrep_xid.cc:107-175` acts on InnoDB only | Store the XID in the RocksDB commit batch; recovery over both engines | Blocker | M-L |
| SST xtrabackup | PXB copies RocksDB; Galera position comes from InnoDB (PXB `src/wsrep.cc:153-171`) | Correct position for RocksDB data; tests | Blocker | M |
| SST clone | Needs section 1 | - | Depends | - |
| IST | Write sets are binlog events | - | Works | - |

Galera has no single-primary mode, so the brute-force abort cannot be avoided as in GR.

Reuse: none. MariaDB's MyRocks has no wsrep code; DDL goes by TOI, DML is not replicated, and its
docs say Galera does not work with MyRocks (W3.8).

## 4. Test plan

- `rocksdb_clone`: Marcin's 10 tests, plus clone under load, restart, and mixed-engine transactions.
- `group_replication`: no test mentions rocksdb today. New set with MyRocks tables in single-primary:
  join by clone and by incremental recovery, failover under load, parallel applier, refusal in
  multi-primary, DDL, XA, savepoints, mixed-engine transactions, crash of primary and secondary.
- `rocksdb_rpl`: stays green; the crash-safe and 2PC tests there are the base for the GR cases.
- `rocksdb`, `rocksdb_sys_vars`: stay green (new sysvars need `_basic` tests).
- PXC, if pursued: `galera`, `galera_3nodes`, `galera_sr`, `galera_nbo` with an engine combination,
  plus SST tests (xtrabackup and clone) with RocksDB data.

## 5. Options

1. Clone first, then GR single-primary. PXC out of scope for now.
2. Option 1, and take PS-11475 as it is (blocking clone) as a documented limitation.
3. Option 1, with a non-blocking clone as a condition for GR.
4. GR and PXC in parallel. PXC needs the engine built there before anything else.

## 6. Open questions for Satya

1. Blocking clone: acceptable for GR, given that it stops writes on the donor (possibly the primary)?
2. Applier lock waits in single-primary: fix (report lock waits, PS-11845, and honour the applier's
   priority) or document timeouts and retries?
3. Where does the single-primary rule live: refuse at DML time in the plugin, or at member start and
   mode switch?
4. Build: keep `GROUP_REPLICATION_WITH_ROCKSDB` as a compile option, or make it always on, or a
   sysvar? Marcin's branch turns it on for release binaries.
5. Which open MyRocks bugs must be fixed first: PS-11832 (DROP crash), PS-11773, PS-11688, PS-11898?
6. Read-free replication and `rocksdb_rpl_skip_tx_api` on GR members: refuse or document?
7. PXC: pursue at all, and if so is "PXC with single writer" an acceptable first limit?
8. Marco Tusa's solution: should I look at it, and who decides between it and Marcin's branch?
9. PS-7782 is On Hold with no reason recorded, and the operator tickets (K8SPS-860/861) say GR +
   MyRocks must be blocked. Do those change with this work?
