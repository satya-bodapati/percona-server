# GTIDs visible to other sessions: a source of truth for ProxySQL

Reference: PS 8.4 (this worktree, c6c14fa65a0). 9.7 and trunk have the same structure (checked
`ordered_commit`, `trx_release_impl_and_expl_locks`). Code walk with excerpts:
`DESIGN-NOTES-gtid-visible-stream-codewalk.md`.

## TL;DR

- The chat is right, and the race starts earlier than stage #2. With `sync_binlog != 1` a dump
  thread can send the GTID right after the flush (stage #1). With `sync_binlog = 1` it can send it
  after the fsync (stage #2). InnoDB makes the change visible only in stage #3.
- `@@global.gtid_executed` is already a correct source of truth. Every path adds a GTID to it only
  after the engine commit. It runs a little behind visibility, at most one commit group, so it never
  reports a GTID too early. What is missing is a push stream of it.
- ProxySQL does not read binlog. It reads a line protocol on `gtid_port` (`ST=<set>`, `I1=uuid:gno`,
  `I2=gno`) and keeps a set, so out-of-order GTIDs are already handled. Anything that speaks that
  protocol can replace `proxysql_binlog_reader` with no change to ProxySQL.
- READ UNCOMMITTED is not a correct fix (§6).

## 1. Commit pipeline, and when each party sees the transaction

`MYSQL_BIN_LOG::ordered_commit` (sql/binlog.cc:9264):

| Step | Where | Who sees it |
|---|---|---|
| Stage #0, replica commit order wait | binlog.cc:9299 | |
| Stage #1, flush. GTIDs are assigned (binlog.cc:8863), the caches are written, and the file is flushed (binlog.cc:9339, :9342) | | |
| `update_binlog_end_pos()` when `sync_binlog != 1` | **binlog.cc:9362** | **dump threads can send the events** |
| Stage #2, optional `binlog_group_commit_sync_delay` wait (binlog.cc:9402), then fsync (binlog.cc:9411) | | |
| `update_binlog_end_pos(file,pos)` when `sync_binlog = 1` | **binlog.cc:9428** | **dump threads can send the events** |
| Stage #3: `call_after_sync_hook` (the semisync AFTER_SYNC wait) | binlog.cc:9469 | |
| The leader commits each THD in turn: `finish_transaction_in_engines` | binlog.cc:8947 | **visible to new read views** (below) |
| `gtid_state->update_commit_group(first)` | binlog.cc:8957 | `gtid_executed`, WAIT_FOR..., session tracker |
| AFTER_COMMIT stage: `after_commit` observers, in order | binlog.cc:8992 | plugins |

If `binlog_order_commits=0` (and clone does not need the commit order), stage #3 is skipped
(binlog.cc:9454). Each thread then commits in `finish_commit` (binlog.cc:9124), and the GTID update
follows at binlog.cc:9131.

The exact point of InnoDB visibility is `trx_commit_low` → `trx_commit_in_memory` →
`trx_release_impl_and_expl_locks`:
- `trx_erase_lists` removes the trx id from `trx_sys->rw_trx_ids` under `trx_sys->mutex`
  (trx0trx.cc:1928, erase at :1886). `ReadView::prepare` copies `rw_trx_ids` under the same mutex
  (read0read.cc:474), so a read view created after this point sees the transaction.
- Locking reads (UPDATE/SELECT FOR UPDATE) wait until the state is COMMITTED_IN_MEMORY and the locks
  are released (trx0trx.cc:1959 and `lock_trx_release_locks` at :2003). Both come right after
  `trx_erase_lists`, inside the same call.
- The redo flush comes later, at `trx_flush_log_if_needed`, and with the binlog it is deferred by
  `flush_log_later` (ha_innodb.cc:6581). Visibility does not wait for durability.

How wide the window is: from binlog.cc:9362 (or :9428) to the engine commit at binlog.cc:8947, for
the last member of the group. In that window are the fsync, `binlog_group_commit_sync_delay` (only
when the end position moves before the sync), the semisync AFTER_SYNC round trip when this replica
is itself a semisync source, the hand-off to the next stage, and the leader's serial engine commits
of earlier group members. That is usually tens of µs to a few ms. It has no upper bound when the
sync delay is set or under semisync AFTER_SYNC. I have not measured it, because this session has no
builds.

On a replica with `log_replica_updates=ON` (the default), the applier goes through the same
`ordered_commit`, so the race is the same.

## 2. GTID state relative to the engine commit

| State | Updated | Exact "visible"? |
|---|---|---|
| `@@global.gtid_executed` | after the engine commit on every path: binlog.cc:8957 (group), :9131 (unordered), handler.cc:1882 (binlog off), rpl_replica_commit_order_manager.cc:377 (commit order, after the engine commit at handler.cc:1981) | **Safe subset.** It lags by up to one group, because the group's GTIDs are added after the last member commits. |
| `WAIT_FOR_EXECUTED_GTID_SET` | waits on `executed_gtids`, which is broadcast per sidno (rpl_gtid_state.cc:317, :920) | same as above. Correct, but it is a blocking call per query. |
| `session_track_gtids` | `notify_after_gtid_executed_update` (rpl_gtid_state.cc:890) | same. ProxySQL already uses it on the writer. |
| `mysql.gtid_executed` | binlog off: a row written inside the trx (handler.cc:1633), or by the InnoDB persister. Binlog on: the persister plus the rotation | it lags by up to 100 ms. You must poll it. |
| `after_commit` observer | binlog.cc:8992, handler.cc:2032, binlog.cc:9146 | **per-trx event after visibility**, carrying the GTID (rpl_handler.cc:813-820) |

## 3. The InnoDB GTID persister (`Clone_persist_gtid`)

- When a trx gets its first undo segment, it is marked to have its GTID persisted
  (trx0undo.cc:1729, then clone0repl.cc:108). With the binlog on, that is every trx whose GTID is not
  ANONYMOUS (clone0repl.cc:142).
- The GTID is written into the undo header inside the commit mtr (trx0trx.cc:1695).
- `get_gtid_info` runs before visibility (trx0trx.cc:1917). `add()` runs **after `trx_erase_lists`**,
  under the serialisation mutex (trx0trx.cc:1995). So any GTID in its in-memory list is already
  visible.
- It keeps two lists, swapped (clone0repl.h:346). A background thread writes them to
  `mysql.gtid_executed` every 100 ms or every 1024 GTIDs (clone0repl.h:330, :336, clone0repl.cc:631).
  After that write it advances `gtid_trx_no` so purge can free the undo (clone0repl.cc:485).
- It cannot feed the stream as it is. It has no subscriber API, its lists are drained and cleared,
  and it covers only InnoDB transactions with undo. It misses MyRocks-only and non-transactional
  transactions, empty transactions and DDL edge cases. Order: `add()` order is the visibility order
  only when commits are serialised (`binlog_order_commits=1`). Otherwise two committers can
  interleave between :1928 and :1995.
- Binlog off, or `log_replica_updates=OFF`: there is no binlog to read at all. `gtid_executed` is
  still updated after the commit (handler.cc:1882), so the same source of truth still works.

## 4. A set or a point

- RPCO=ON, `binlog_order_commits=ON`, single source: stage #0 orders the flush in relay-log order,
  and stage #3 commits in flush order. Per source UUID, visibility is therefore in source order with
  no gaps. "Last visible GTID per UUID" is well defined.
- RPCO=OFF (the default is ON, sys_vars.cc:4261-4267), `binlog_order_commits=0`, or binlog off: workers
  commit in any order, so there are gaps and only a **set** is correct.
- Multi-source, local writes and tagged GTIDs (8.4) all add UUIDs or tags. The state is per
  UUID(:tag) in every case.
- What a stream can promise: every GTID it sends is already visible to a new statement. It cannot
  promise an order across transactions. ProxySQL's `addGtid` already merges a set.

## 5. Candidate designs

| | Correct | Commit-path cost | ProxySQL change | Outside server tree |
|---|---|---|---|---|
| **A. Reader-side wait.** `proxysql_binlog_reader` holds each batch until `WAIT_FOR_EXECUTED_GTID_SET(batch)` on a second connection returns | yes | none | none (reader only) | yes, no server change |
| **B. Daemon plugin with an `after_commit` observer** that queues GTIDs and serves ProxySQL's `gtid_port` protocol (`ST=` from `gtid_executed`, then `I1`/`I2`) | yes (visible before the hook runs) | one non-blocking enqueue per trx | none: point `gtid_port` at the plugin | a plugin built against the server source (same as semisync). I have not checked whether a component service exists. |
| **C. Fake binlog plugin** (Marcos's idea): the same hook, but it serves the binlog dump protocol with GTID+BEGIN/COMMIT | yes | same as B | none, the reader stays unchanged | plugin. It means reimplementing the dump protocol (FDE, rotate, heartbeats, checksums, GTID auto-position), which is more work than B for the same data. |
| **D. Visible-only dump thread.** A per-connection flag makes `Binlog_sender` send a GTID only after it is in `gtid_executed` | yes | none on commit. The dump thread waits | the reader sets a session var before dumping | no, this is a server change |
| **E. Move `update_binlog_end_pos` after stage #3** (global) | yes | delays every replica | none | no. **Deadlocks semisync AFTER_SYNC**, because the ack is awaited before the engine commit (binlog.cc:9469). Rejected. |
| **F. Poll the persister or `mysql.gtid_executed`** | yes, with lag | none | — | polling, which Marcos rules out |

On reconnect, all of them resync with `ST=` from `gtid_executed`. Events that the stream does not
see, such as `SET gtid_purged` and `RESET`, are covered by that resync.

## 6. READ UNCOMMITTED

It closes this race: the applier wrote the rows before prepare, so a dirty read sees them. But it
is not correct as a fix:
- It sees transactions part-way through. A half-applied multi-statement trx gives torn reads, and
  rows that an applier later rolls back (deadlock retry, error) are read as if they existed.
- It changes the semantics of every read on the reader, not only the causal ones.
- It does nothing for metadata or DDL visibility, and it is ignored by engines or paths that build
  their own snapshot.

So it trades "too old" for "never committed".

## 7. What ProxySQL does today

`proxysql_binlog_reader` runs on the MySQL host and connects as a replication client. It takes a
snapshot from `Executed_Gtid_Set`, then streams the binlog. It serves a text protocol on a port
(default 6020, `mysql_servers.gtid_port`): `ST=<gtid set>\n`, then `I1=<uuid>:<gno>\n` or
`I2=<gno>\n` (same UUID as the previous line). ProxySQL's `GTID_Server_Data::read_next_gtid` parses
these lines into a per-UUID interval set (`addGtid`). The writer's GTID comes from
`session_track_gtids=OWN_GTID`. Sources: the github.com/sysown/proxysql_mysqlbinlog README and
proxysql lib/GTID_Server_Data.cpp. Two things to check: the parser has no tag support, and 8.4
removed `SHOW MASTER STATUS`.

## Open questions for Satya and Marcos

1. Is a fix on the ProxySQL side acceptable (A, with zero server work), or must the server own it
   (B, C or D)?
2. If the server owns it, should the plugin speak ProxySQL's `gtid_port` line protocol (B) or the
   binlog protocol (C)? B is smaller, and C keeps the existing reader.
3. Must the stream also support `binlog_order_commits=0` / RPCO=OFF / binlog-off replicas, or only
   the default configuration?
4. Is a plugin built in the PS tree fine, or does it have to be a component? If it has to be a
   component, someone needs to check whether a transaction-observer service exists.
5. Should we measure the window (Debug sync point and a sysbench run on the lab) before choosing?
6. Tagged GTIDs: ProxySQL's parser cannot read them. Is that in scope?
