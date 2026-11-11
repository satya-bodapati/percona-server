# Visible GTID Stream

Design proposal. Percona Server 8.4 (line numbers at c6c14fa65a0). Not implemented; nothing was built
or measured. Names of variables, options and counters are proposals.

A stream from the server that reports a GTID only after its changes can be read by other sessions, so
a proxy can safely route "read your own write" queries to a replica.

## The design in five lines

1. The binary log gets a second marker, **V**, the *visible position* (last committed position).
   Everything in the binlog before V is committed in the storage engines.
2. The group-commit leader moves V once per commit group, right after the engine commits and the
   `gtid_executed` update.
3. A consumer opens a normal replication connection (`COM_BINLOG_DUMP_GTID`) with one extra option.
   Its dump thread reads only up to V and sends only GTID events.
4. Every connection starts with a full snapshot of the visible set. Reconnect, reset and crash all
   reduce to "connect again, replace your set".
5. Requires `gtid_mode=ON`, the binlog on, `log_replica_updates=ON` on replicas, and
   `binlog_order_commits=ON`. All of these are defaults in 8.4.

## 1. Why we are solving this

An application writes a row through ProxySQL and reads it back straight away. The write goes to the
primary. If the read goes to a replica that has not applied the write yet, the application does not
see its own row.

ProxySQL solves this with GTIDs, in two halves:

- **What did this client write?** The primary returns the GTID of each commit in the OK packet
  (`session_track_gtids=OWN_GTID`). ProxySQL remembers the last one per client.
- **Which servers have it?** ProxySQL keeps, in memory, the set of GTIDs each backend has executed.
  A read is sent only to a backend whose set contains the client's last GTID.

The second half needs a feed from every backend, fast enough to route thousands of reads per second
without asking the server each time. Today that feed is `proxysql_binlog_reader`: a small process
beside each MySQL server that reads the server's binlog as a replica would and pushes each GTID it
sees to ProxySQL.

```
                         1. write; OK packet carries GTID
 Application --> ProxySQL ---------------------------------> Primary
                 (GTID set                                      |
                  per backend) --- 3. read, if GTID in set -+   | replication
                      ^                                     |   v
                      +---- 2. feed: GTIDs visible ------- Replica
```

Step 2 is the subject of this design. Today the feed comes from a binlog reader and can run ahead of
what a query on the replica can see.

## 2. The problem: the binlog runs ahead of visibility

The reader reports a GTID when the transaction appears in the replica's binlog. A transaction appears
in the binlog *before* it is committed in InnoDB. For a short time the reader says "this replica has
GTID 5", ProxySQL sends the read there, and the row is not there yet.

```
One commit group, time runs left to right

 | 1 Flush        | 2 Sync       | semisync wait   | 3 Commit         | client gets OK
 | write to file  | fsync binlog | (if AFTER_SYNC) | engine commit    |
                  ^              ^                                    ^
                  |              |                                    |
       binlog_end_pos      binlog_end_pos                  row visible, then
      (sync_binlog != 1)  (sync_binlog = 1)                gtid_executed, then V

                  |<--- reader has reported the GTID; a SELECT here --->|
                  |     does not see the row                            |
```

Dump threads may read up to `binlog_end_pos`. A process tailing the file can see bytes even earlier,
from the write in stage 1. Today there is one marker, `binlog_end_pos`, and it moves before the engine
commit. The proposal adds V, which moves after it.

### Why the binlog is ahead: three stages

- **The binlog decides the commit.** A transaction is prepared in InnoDB, written to the binlog, and
  only then committed in InnoDB. After a crash, a prepared transaction is committed if it is in the
  binlog and rolled back if not. So "in the binlog" must come before "committed".
- **Group commit.** Many sessions share one binlog write and one fsync (stages 1 and 2). Engine
  commits follow as a batch in stage 3. This is where the throughput comes from.
- **Replication wants events early.** With semisync `AFTER_SYNC`, the source must send the group to a
  replica and get an acknowledgement *before* the engine commit. So dump threads must be allowed to
  read before stage 3. That is why `binlog_end_pos` cannot simply move later: semisync would wait for
  an event that can never be sent.

`binlog_end_pos` answers "is this safe to replicate?". ProxySQL asks a different question: "can a
query see this?". Different questions need different markers.

**How large is the window?** Normally microseconds to a few milliseconds. It has no upper bound when
`binlog_group_commit_sync_delay` is set, when the fsync is slow, or when a semisync acknowledgement is
slow. A proxy that routes a read within a millisecond of the write hits it.

Code: sql/binlog.cc flush :9339, end_pos :9362 and :9428, after-sync hook :9469, engine commits :8947,
gtid_executed :8957. storage/innobase/trx/trx0trx.cc:1928 (row becomes visible).

## 3. Requirements

| #  | Requirement | How the design meets it |
|----|-------------|-------------------------|
| R1 | Never report a GTID before a new query on this server can see its changes. | V moves after the engine commits of the group. |
| R2 | Report it soon after; no polling. | Leader signals a condition variable; stream threads wake and send. |
| R3 | Committing sessions never wait for a consumer. | No queue. Consumers read the binlog file at their own pace. |
| R4 | Several consumers, connected at different times. | One dump thread per consumer; its only state is a file position. |
| R5 | A consumer needs no saved state to start or restart. | Every connection begins with a full snapshot of the visible set. |
| R6 | Any third party can parse it with an existing binlog library. | Standard replication protocol and standard event types. |
| R7 | Consumer can tell "idle" from "dead". | Standard heartbeat events. |
| R8 | GTID resets, purges and restarts do not leave a wrong set behind. | Connection is closed or a new snapshot is sent; the consumer replaces its set. |

**How ProxySQL would use it.** Two options, same stream:

- Smallest change: `proxysql_binlog_reader` stays, sets the one new option before it starts the dump,
  and treats the first event as a full set. ProxySQL itself is unchanged.
- Later: ProxySQL connects to MySQL directly and the reader process goes away. This needs a binlog
  client and tagged-GTID support inside ProxySQL.

## 4. Primary or replica, and which settings

The stream describes the server it runs on. It belongs on **every server that receives causal reads**,
which in practice means the replicas. That is where the wrong reads happen today.

On the primary it also works, and ProxySQL needs a GTID set for the primary too, since reads can be
routed there. But the primary has no correctness problem today: the client only learns its GTID from
the OK packet, and the OK packet is sent after the commit. Using the same stream on the primary keeps
one mechanism for all backends.

| Setting | Primary | Replica | Why |
|---------|---------|---------|-----|
| `gtid_mode` | ON | ON | No GTIDs otherwise. |
| `log_bin` | ON | ON | The stream is read from this server's own binlog. |
| `log_replica_updates` | any | ON | Applier transactions must be written to the replica's binlog. |
| `binlog_order_commits` | ON | ON | Engine commit order must equal binlog order, or one marker cannot describe what is committed. |
| `session_track_gtids` | OWN_GTID | any | ProxySQL's writer half. Not part of this feature. |
| `replica_parallel_workers` | n/a | any | Workers commit through the same group commit. |
| `replica_preserve_commit_order` | n/a | any | When OFF, GTIDs arrive out of numeric order. The consumer keeps a set, so this is fine. |
| `sync_binlog`, `innodb_flush_log_at_trx_commit` | any | any | Durability only. See crash handling in section 10. |
| semisync, `binlog_group_commit_sync_delay` | any | any | They widen today's window. V is after them. |

If a required setting is off, the server refuses the stream with an error that names the setting. It
never falls back to the ordinary binlog stream.

Not supported by this design: replicas with the binlog off or `log_replica_updates=OFF`. Such replicas
exist in the field. Supporting them needs a different source (a bounded ring fed where `gtid_executed`
is updated), discussed earlier and set aside by the assumption for this document.

## 5. Source of truth: the visible position V

V is a binlog coordinate (file name, offset) with one meaning: **every transaction that ends at or
before V is committed in the engines and present in `gtid_executed`.**

```
replica binlog file

 | T1 | T2 | T3 | T4 | T5 | T6 | T7 |   not written yet
 |<----- committed ------>|<-flushed->|
                          ^           ^
                          V (new)     binlog_end_pos

 replication dump thread: sends whole events up to binlog_end_pos  ---------------->|
 visible stream thread:   reads up to V, sends GTID events only    ----->|
```

T1-T5 are committed in InnoDB. T6-T7 are in the binlog but a SELECT cannot see them yet. The existing
marker and the existing dump threads are unchanged.

### Who moves V, and when

The leader of the commit stage. In `MYSQL_BIN_LOG::process_commit_stage_queue`, while it still holds
`LOCK_commit`:

```
for each session in the group:  finish_transaction_in_engines()   // rows become visible   :8947
gtid_state->update_commit_group(first)                            // gtid_executed updated :8957
update_binlog_last_committed_pos(file, pos of the group's last session)   // NEW: store V, wake stream threads
```

The end position of every session is already recorded at flush time (`THD::set_trans_pos`,
binlog.cc:8764). The sync stage already walks the group to find the last one for `binlog_end_pos`
(:9419-9428). V uses the same value, later.

### Why this is correct

- **Order.** With `binlog_order_commits=ON`, groups pass through the commit stage in the order they
  were flushed. When the group ending at V has committed, every earlier group has too. V only moves
  forward.
- **Every GTID in the binlog passes this point.** Client transactions, applier transactions, DDL,
  empty transactions made by replication filters, XA, and statements written with
  `write_stmt_directly` all reach the binlog through `ordered_commit` (binlog.cc:7905-7916 calls
  `commit()`).
- **Never ahead of `gtid_executed`.** V is stored after the GTID set is updated, under the same lock.
  A snapshot of the set taken after reading V always covers everything before V.
- **Binlog rotation.** Rotation waits only for transactions that wrote an XID (`wait_for_prep_xids`,
  :6998). A group without one can be flushed in the old file and committed after the rotation. So V
  carries the file name, and a reader in an older file than V reads that file to its end; a reader in
  V's file stops at V's offset.
- **Server start.** Crash recovery finishes before connections are accepted, so V starts at the end
  of the newly opened binlog.

### GTIDs that never reach the binlog

A few rare operations add GTIDs to `gtid_executed` without writing a transaction to the binlog:
`SET GLOBAL gtid_purged`, a session with `sql_log_bin=0` and an explicit `gtid_next`, and
`RESET BINARY LOGS AND GTIDS` (which removes GTIDs). For these the server keeps a **resync counter**.
Each such operation increments it and wakes the stream threads; each thread then sends a fresh
snapshot (section 8). The counter is one atomic increment on paths that are already rare and slow.

Code: sql/rpl_gtid_state.cc add_lost_gtids :656, clear :60. sql/handler.cc:1882 (binlog-off
update_on_commit).

## 6. Stream threads and slow readers

There is no separate producer thread and no queue. The "producer" is the binlog itself plus V.

- **One thread per consumer.** It is the consumer's own connection thread, taken over by
  `Binlog_sender` exactly as for a replica. State: an open binlog file and an offset.
- **Waiting.** When the thread reaches V it sleeps on a new condition variable and wakes when the
  leader moves V, when the resync counter changes, or when the heartbeat period ends.
- **A slow consumer** is simply further back in the file. It holds no memory in the server and slows
  nobody. A blocked socket blocks only its own thread.
- **The one side effect** of a very slow consumer is the existing one for slow replicas: the binlog
  file it is reading cannot be purged (`log_in_use`, binlog.cc:7816). We do not solve this now. We
  count it (section 11) and decide later whether a thread that falls several files behind should jump
  forward with a new snapshot, which this design allows at any time.

First version: the thread reads each event and drops everything that is not a GTID event. Reading
only the 19-byte event header and seeking past the body is a later optimisation; it changes nothing
in the protocol.

## 7. Cost on the commit path

| What | How often | Under which lock |
|------|-----------|------------------|
| Store V (file name pointer + offset) | once per commit group | new mutex `LOCK_binlog_last_committed_pos`, taken inside `LOCK_commit` |
| Wake stream threads | once per group, skipped when no stream is connected | same |
| Find the group's end position | inside the loop that already walks the group | none added |

### Why a new mutex, when the leader already holds `LOCK_commit`

The mutex is for the stream threads, not for the committers. Only one leader is in the commit stage at
a time, so writers of V never compete with each other. The stream threads need it for two things:

- **Sleeping without missing a wake-up.** A stream thread checks "has V moved past my position?" and,
  if not, sleeps on a condition variable. If the leader moved V and signalled between the check and
  the sleep, the thread would sleep through it and the GTID would wait until the next commit. A
  condition variable avoids this only when the check, the sleep, and the update all happen under one
  mutex.
- **Reading a matching pair.** V is a file name and an offset. Without a lock, a thread could read
  the new file with the old offset during a rotation.

### Why not reuse `LOCK_commit`

It would work: the leader already holds `LOCK_commit` when it moves V, so stream threads could wait on
a condition tied to it and no new mutex would be needed. We do not, for three reasons.

- **Stream threads would slow commits.** The next leader must acquire `LOCK_commit` to enter the
  commit stage. After each group the leader wakes all stream threads, and each of them must
  re-acquire the mutex to wake up. With `LOCK_commit`, that crowd lines up for the same lock as the
  next commit group, at the moment it wants it. More consumers would mean slower commits, which
  breaks requirement R3.
- **Commits would slow stream threads.** `LOCK_commit` is held for the whole commit stage, across the
  engine commit of every transaction in the group. A stream thread that only wants to compare two
  positions would wait behind all of that.
- **Lock ordering.** `LOCK_commit` is part of the group-commit lock chain (`LOCK_log`, `LOCK_sync`,
  `LOCK_commit`, `LOCK_after_commit`) and is also taken by code that must stop commits, such as
  `get_gtid_executed` and `MYSQL_BIN_LOG::xlock` (binlog.cc:7785, :9613). Bringing consumer threads
  into that chain adds deadlock cases to reason about. A leaf mutex that is only ever taken last, and
  held for a few instructions, adds none.

With a separate mutex the leader holds it for a few instructions, and a stream thread holds it only to
compare positions and go to sleep. Consumers stay off the commit path's locks.

### Same strategy as `LOCK_binlog_end_pos`

Replication already solves this exact problem for `binlog_end_pos`: it has its own
`LOCK_binlog_end_pos` and condition variable, separate from `LOCK_log` and `LOCK_sync`. The leader
takes it once per group to store the position and signal; dump threads take it only to wait
(binlog.cc:9775-9792, rpl_binlog_sender.cc:790-812). We follow the same strategy and name the new
pieces to match.

| Role | Existing, for replication | New, for the visible stream |
|------|---------------------------|-----------------------------|
| Position | `atomic_binlog_end_pos` (offset in the active file) | last committed position, V (file name + offset) |
| Mutex | `LOCK_binlog_end_pos` | `LOCK_binlog_last_committed_pos` |
| Condition | `update_cond` | `update_last_committed_cond` |
| Writer | `update_binlog_end_pos()`, after flush or sync | `update_binlog_last_committed_pos()`, after the engine commits |
| Waiter | `Binlog_sender::wait_new_events()` | the same function in visible mode, on the new mutex and condition |

One difference: `binlog_end_pos` is a single atomic offset, so dump threads read it without the mutex
and lock only to sleep. V also carries a file name, so the first version reads it under the mutex. A
lock-free read can be added later if a profile shows it matters.

### Summary of the cost

- It mirrors what `update_binlog_end_pos` already does once per group for replication (:9785-9792).
- It happens *after* the engine commits, so it does not delay visibility or lock release.
- Nothing is allocated, copied or written per transaction. No file or network I/O happens under the
  new mutex. Stream threads hold it only to go to sleep.
- A committing session can never be blocked by a consumer. This is the property the earlier queue and
  ring designs had to work for; here it follows from having no queue.

The only heavier operation is the snapshot of `gtid_executed`, once per connection and per resync. It
is taken with the read lock and one per-source mutex at a time, the same locks
`WAIT_FOR_EXECUTED_GTID_SET` uses (rpl_gtid_state.cc:384-386), not the global write lock that
`SELECT @@global.gtid_executed` takes (sys_vars.h:2775).

## 8. Consumer: register, format, parse

### Registering

A normal MySQL connection, a user with `REPLICATION SLAVE`, then the same steps a replica performs.
Options are passed as user variables, which is the existing convention for this command (heartbeat,
checksum and semisync all work this way).

```
SET @source_binlog_checksum  = 'NONE';          -- or CRC32; as a replica does
SET @source_heartbeat_period = 1000000000;      -- nanoseconds; 1 s
SET @replica_uuid            = '<unique per consumer>';
SET @visible_gtid_stream     = 1;               -- NEW: the only new step

COM_BINLOG_DUMP_GTID  server_id = <unique, not 0>, file = "", pos = 4, gtid set = empty
```

- **Unique UUID and server id.** A new dump connection with the same UUID kills the old one
  (`kill_zombie_dump_threads`, rpl_source.cc:1192). Server id 0 makes the server stop at the end of
  the log instead of waiting (rpl_binlog_sender.cc:354).
- **The position and GTID set in the request are ignored.** The server always starts at V with a
  snapshot.
- **Check support first.** An older server ignores the new variable and sends the ordinary binlog.
  The consumer must confirm the feature exists before it sends the dump command (proposed: the status
  variables in section 11 exist only on servers that have it).

### What the server sends

| Event | Type code | When | Consumer action |
|-------|-----------|------|-----------------|
| Rotate (artificial) | 4 | first event; later at each new binlog file | parser bookkeeping only |
| Format description | 15 | after each Rotate | parser bookkeeping only |
| **Previous GTIDs** | 35 | once after connect; again after each resync | **replace** the set with this one |
| **GTID** / tagged GTID | 33 / 42 | one per transaction that became visible | **add** `uuid[:tag]:number` to the set |
| Heartbeat | 27 / 41 | when idle for the heartbeat period | server is alive; nothing new |
| Error packet | - | refused, killed, reset, settings changed | mark the set unknown; reconnect |

- All are existing binlog event types with their existing byte layout, so any binlog client library
  can decode them. Row events, query events and transaction payloads are never sent.
- The Previous GTIDs event here is built by the server from the snapshot. The ones stored at the
  start of each binlog file are not forwarded, so the rule "Previous GTIDs means replace" has no
  exceptions.
- GTID events are sent as stored. With parallel appliers and `replica_preserve_commit_order=OFF` the
  numbers arrive out of order.

### Connect sequence inside the server

1. Check privileges and the required settings; refuse with a clear error if one is off.
2. Read V. This is where the thread will start reading.
3. Copy `gtid_executed`. It covers everything before V, and possibly a little more.
4. Send Rotate, Format description, Previous GTIDs (the copy).
5. Loop: send GTID events up to V; at V, wait.

A GTID that committed between steps 2 and 3 is in the snapshot and is also sent again as an event.
Adding a GTID twice is harmless, so no lock has to cover the two steps together.

### Consumer loop

```
visible = unknown
on PREVIOUS_GTIDS(set):  visible = set
on GTID(uuid, tag, n):   visible.add(uuid, tag, n)
on HEARTBEAT:            last_seen = now
on error / EOF / no packet for 2 x heartbeat period:
                         visible = unknown; reconnect with backoff

route a causal read here only if visible is known and contains the client's GTID
```

### Parsing and tools

The data is binary, but no new parser is needed. Every reader of this stream is already a binlog
reader: `proxysql_binlog_reader` receives the same binary events today and turns each GTID into the
text line it sends to ProxySQL. Any existing binlog library can decode the stream: the C client API
(`mysql_binlog_open` / `mysql_binlog_fetch`), go-mysql, python-mysql-replication, the Java binlog
connector, and so on.

What a consumer has to change:

- Set `@visible_gtid_stream = 1` on the connection before starting the dump. Some libraries hide the
  connection setup, so this may need a small patch or an option in the library.
- Treat a Previous GTIDs event as "replace my set". Most readers ignore that event today.
- Tagged GTIDs (event type 42) need a library recent enough to know them.

`mysqlbinlog` needs no parser change, only a new option to ask for the stream. It already sets its own
variable before a remote dump (`@source_binlog_checksum`, client/mysqlbinlog.cc:2551), so a
`--visible-gtids` option would add one more `SET` there. Not checked: how its printer behaves when a
GTID event is not followed by a transaction.

ProxySQL itself does not need to parse binary if the reader process stays: the reader keeps speaking
its text protocol to ProxySQL and only its upstream side changes.

## 9. How a consumer finishes

- It closes the connection. There is nothing to unregister and no server-side state to clean up
  beyond the thread.
- The server notices on its next write to the socket, which is at the latest the next heartbeat. This
  is why a heartbeat period should always be set.
- An administrator can end a stream with `KILL`. Streams appear in `SHOW PROCESSLIST` as
  `Binlog Dump GTID` with their own state text (proposed: "Sending visible GTIDs").

## 10. Reconnect, reset and other events

One rule covers almost everything: **a new connection starts with a full snapshot, and the consumer
replaces its set.**

| Event | Server | Consumer |
|-------|--------|----------|
| Network drop, consumer restart | thread ends | reconnect; set replaced |
| Server restart or crash | connection lost; after recovery V starts at the end of the new binlog | reconnect; set replaced. Transactions lost with non-durable settings disappear from the set because it is replaced, not merged. |
| Binlog rotation | thread follows into the next file | nothing |
| Binlog purge | files a stream is reading are kept, as for replicas | nothing |
| `RESET BINARY LOGS AND GTIDS` | resync counter bumped; streams are closed with an error | reconnect; set replaced (now smaller) |
| `SET GLOBAL gtid_purged`, GTID committed with `sql_log_bin=0` | resync counter bumped; each stream sends a new Previous GTIDs event | set replaced; connection stays |
| Required setting turned off at run time | streams are closed with an error naming the setting | backend has no known set until fixed |
| Replica promoted to primary | nothing; the stream describes this server whatever its role | nothing |
| Consumer falls far behind | counted; no action in the first version | may reconnect to jump to the present |

While a consumer has no connection, or has not yet received the first Previous GTIDs event, it must
treat the backend's set as unknown and not route causal reads there.

## 11. Observability

Following the earlier decision to observe slow readers before solving them.

| Status variable (proposed) | Meaning |
|----------------------------|---------|
| `Binlog_visible_file`, `Binlog_visible_pos` | V. Compare with `SHOW BINARY LOG STATUS` to see the gap this design closes. |
| `Binlog_visible_stream_clients` | connected streams |
| `Binlog_visible_stream_gtids_sent` | GTID events sent, all streams |
| `Binlog_visible_stream_snapshots_sent` | snapshots sent (connects + resyncs) |
| `Binlog_visible_stream_max_lag_bytes` | distance from V of the slowest stream |

Error log: one line when a stream connects, is refused (with the setting), is closed by a reset or a
setting change, and when a stream is more than one binlog file behind V.

## 12. Code change map and tests

| File | Change |
|------|--------|
| `sql/binlog.h`, `sql/binlog.cc` | V (file, offset), `LOCK_binlog_last_committed_pos` and its condition; `update_binlog_last_committed_pos()` called from `process_commit_stage_queue`; initial value at open and after reset. |
| `sql/rpl_binlog_sender.h/.cc` | Visible mode: end position from V instead of `binlog_end_pos` (`get_binlog_end_pos` :538, `wait_new_events` :790); send only GTID events; snapshot as Previous GTIDs; resync check. |
| `sql/rpl_source.cc` | Read `@visible_gtid_stream` in `com_binlog_dump_gtid` (:1011); settings check; skip the start-file search. |
| `sql/rpl_gtid_state.cc`, `sql/handler.cc` | Bump the resync counter where GTIDs change outside the binlog. |
| `sql/mysqld.cc` | Status variables. |
| `client/mysqlbinlog.cc` | `--visible-gtids` option (test and debugging client). |

InnoDB is not touched. The existing dump threads, `binlog_end_pos` and semisync are not touched.
9.7 and trunk have the same structure with different line numbers.

### Testing, in three layers

1. **SQL only, through the status variables.** Pause a commit at the existing sync point
   `bgc_after_sync_stage_before_commit_stage` (binlog.cc:9431). `SHOW BINARY LOG STATUS` has moved
   past the transaction, `Binlog_visible_pos` has not, and a SELECT does not see the row. After
   release, all three agree. This tests where V moves, with no stream client.
2. **mysqlbinlog with `--visible-gtids` as the stream client.**
   - Without `--stop-never` it asks the server to stop at the end instead of waiting. In visible mode
     the end is V, so it prints the snapshot plus everything up to V and exits. Deterministic: run it
     while the commit is paused and the GTID is absent; run it after release and the GTID is there.
   - With `--stop-never`, in the background, writing to a file: tests the live path (wake-up,
     rotation in the middle of a group, resync after `SET gtid_purged`, reset, two consumers, parallel
     applier with commit order off, filtered empty transactions, refused settings).
3. **A small C program on libmysqlclient, as a follow-up.** `mysql_binlog_open` /
   `mysql_binlog_fetch`, about 100-150 lines. It serves as a reference consumer for third parties and
   as a stress test that checks the promised property under load: every GTID the stream reported is
   visible to a SELECT on a second connection.

End to end, outside MTR: the real `proxysql_binlog_reader` with the option set, behind a ProxySQL.

## 13. Limits, unverified points, decisions

### Limits accepted by the assumption

- Binlog-less replicas and `log_replica_updates=OFF` are not served.
- `binlog_order_commits=OFF` is not served.
- The stream says what is visible on this server. It says nothing about durability.

### Not yet verified in code (before implementation)

- What an existing dump thread does on `RESET BINARY LOGS AND GTIDS`, so the visible stream can close
  in the same way.
- Turning `binlog_order_commits` off and on at run time. Sessions that are committing unordered at
  the moment of the switch are not covered by V. Closing streams on OFF is clear; the rule for when a
  stream may start again after ON needs to be worked out.
- XA transactions: the GTID of `XA PREPARE` is reported although nothing is visible yet. Harmless for
  routing, but should be stated.
- Group Replication and PXC commit paths were not read for this document.
- How mysqlbinlog prints a GTID event that has no transaction after it.
- Nothing was built or measured. The window size and the cost in section 7 are from reading code.

### Decisions for Satya and Marcos (open)

1. Confirm the assumption: binlog on, `log_replica_updates` on, ordered commits.
2. Opt-in by user variable (as written) or by a new flag bit in the dump command. Both are ignored by
   old servers, so a support check is needed either way.
3. How a consumer checks support: status variable, a read-only system variable, or version.
4. Privilege: keep `REPLICATION SLAVE`, or a narrower one since no row data is sent.
5. First consumer: the existing reader with one new option, or ProxySQL directly.
6. Tagged GTIDs: the server sends them; ProxySQL's set does not understand them today.
