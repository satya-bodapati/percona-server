# Code walk: commit → visibility → GTID state (PS 8.4)

Companion to `DESIGN-NOTES-gtid-visible-stream.md`. Every line below was checked by printing it in
this worktree (c6c14fa65a0).

## A. Binlog group commit (sql/binlog.cc)

```
ordered_commit                                    :9264
  Stage #0  Commit_order_manager::wait_for_its_turn_before_flush_stage   :9299
  Stage #1  change_stage(BINLOG_FLUSH_STAGE)                             :9316
            process_flush_stage_queue                                    :9339
              assign_automatic_gtids_to_flush_group                      :8863
            flush_cache_to_file                                          :9342
            RUN_HOOK(binlog_storage, after_flush)                        :9356
            if (sync_binlog != 1) update_binlog_end_pos()   <-- dump can send   :9362
  Stage #2  change_stage(SYNC_STAGE)                                     :9386
            wait_count_or_timeout(binlog_group_commit_sync_delay)        :9402
            sync_binlog_file                                             :9411
            if (sync_binlog == 1) update_binlog_end_pos(file,pos) <-- dump can send :9428
  Stage #3  (only if binlog_order_commits || clone needs order)          :9454
            call_after_sync_hook(commit_queue)    semisync AFTER_SYNC    :9469
            process_commit_stage_queue                                   :9486
              for each THD: finish_transaction_in_engines   <-- InnoDB visible :8947
              gtid_state->update_commit_group(first)         <-- gtid_executed :8957
            AFTER_COMMIT stage: RUN_HOOK(transaction, after_commit)      :8992
  signal_done; finish_commit
            unordered path: finish_transaction_in_engines :9124, update_on_commit :9131,
                            after_commit hook :9146
```

The dump thread reads up to `mysql_bin_log.get_binlog_end_pos()` (rpl_binlog_sender.cc:549) and
waits for `signal_update()` (binlog.cc:9767, rpl_binlog_sender.cc:790). An external process
reading the file can see bytes even earlier, from the moment of `flush_cache_to_file` (:9342).

## B. InnoDB visibility (storage/innobase/trx/trx0trx.cc)

```
innobase_commit            ha_innodb.cc:6485   (flush_log_later = true :6581)
 innobase_commit_low       ha_innodb.cc:6338
  trx_commit / trx_commit_low                         :2209
   trx_write_serialisation_history                    :2243
     trx_undo_gtid_set (GTID into the undo header)    :1695
   trx_commit_in_memory                               :2297
    trx_release_impl_and_expl_locks                   :1901
      get_gtid_info (outside mutex)                   :1917
      trx_sys_mutex_enter; trx_erase_lists            :1928   <-- new ReadView sees it
          rw_trx_ids.erase                            :1886
      state = COMMITTED_IN_MEMORY (shard latch)       :1959   <-- implicit locks released
      serialisation mutex: gtid_persistor.add         :1995
                           erase from serialisation list
      lock_trx_release_locks                          :2003   <-- explicit locks released
```

`ReadView::prepare` (read/read0read.cc:462) runs under `trx_sys->mutex` and copies `rw_trx_ids`
(:474). It also sets `m_low_limit_id = trx_sys_get_next_trx_id_or_no()` (:470). A trx committed
before `prepare` is therefore either absent from `m_ids` or below `m_up_limit_id`, so it is visible.

The comment at trx0trx.cc:1974-1979 spells out the required order: erase from `rw_trx_ids` before
leaving the serialisation list.

## C. GTID state (sql/rpl_gtid_state.cc)

- `update_commit_group` :170. Asserts that the caller owns `LOCK_commit`. Adds all GTIDs of the
  group and broadcasts per sidno (:211).
- `update_gtids_impl_own_gtid`: `executed_gtids._add_gtid` (:889), then
  `notify_after_gtid_executed_update`, which drives `session_track_gtids` (:890).
- `wait_for_gtid_set` :317 (`WAIT_FOR_EXECUTED_GTID_SET`) waits on the per-sidno cond that
  `update_gtids_impl_broadcast_and_unlock_sidno` (:920) signals.

Paths where the GTID is added after the engine commit:
1. Group commit: binlog.cc:8947, then :8957.
2. `binlog_order_commits=0`: binlog.cc:9124, then :9131.
3. Binlog off / `sql_log_bin=0` / `log_replica_updates=0`: `commit_owned_gtids` saves the table row
   before prepare (handler.cc:1591, :1633). `update_on_commit` runs after `ha_commit_low`
   (handler.cc:1882).
4. Replica commit order without binlog: `ht->commit` (handler.cc:1981), then `wait_and_finish`
   (:2008), then `flush_engine_and_signal_threads`, then `update_commit_group`
   (rpl_replica_commit_order_manager.cc:377).

## D. InnoDB GTID persister (storage/innobase/clone/clone0repl.cc)

- `set_persist_gtid(trx,true)` at the first undo assignment (trx0undo.cc:1729). With the binlog on,
  it skips only ANONYMOUS (clone0repl.cc:142). With the binlog off, it needs ASSIGNED.
- `add()` :45. Pushes onto the active list. Wakes the thread at 1024 entries (:75). Blocks the
  committer once 1M entries are reached (:59).
- `periodic_write()` :588. Wakes every 100 ms (:631). `flush_gtids` :502 swaps the lists
  (`switch_active_list`) and writes them through `gtid_table_persistor->save` (:420-482). Then it
  calls `update_gtid_trx_no(oldest_trx_no)` (:485), which persists the number and wakes purge.
- Purge must wait for the persister, because the undo still holds GTIDs that have not been written
  yet (trx0purge.cc:2587).

## E. Observer API

`Trans_delegate::after_commit` (rpl_handler.cc:806) fills `Trans_param.gtid_info` from
`last_used_gtid_tracker_ctx` (:813-820). It calls the plugin observers (:835) and also `se_after_commit` in
the storage-engine plugins (:836). The struct is in sql/replication.h:136. The callback type is
`after_commit_t` (:270).

## F. Same structure in 9.7 and trunk

`git show origin/9.7:` and `origin/trunk:`. In both, `update_binlog_end_pos` comes before the commit
stage, and `trx_erase_lists` comes before `gtid_persistor.add`. Only the line numbers differ
(9.7 binlog.cc:8333/7928, trunk :7954/7539).
