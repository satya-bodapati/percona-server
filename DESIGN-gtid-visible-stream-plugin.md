# Visible GTID Stream Plugin

Plugin design. No server change. Percona Server 8.4 and upstream MySQL. Proposal, not implemented.

A plugin that pushes each GTID to a client the moment a query on this server can see it. Installable
on an unmodified server.

## 1. The problem

ProxySQL sends a read to a replica only if the replica already has the client's last write,
identified by its GTID. It learns which GTIDs a replica has from a helper that reads the replica's
binlog.

A transaction reaches the binlog *before* it is committed in InnoDB. So the helper reports the GTID
too early, and for a short time a read sent to that replica does not see the row.

```
 | 1 Flush              | 2 Sync       | 3 Commit          | after_commit hook
 | write to binlog file | fsync binlog | commit in InnoDB  | plugin is called here
                        ^                                  ^
                        |                                  |
                 today's reader                       this plugin
              reports the GTID here              reports the GTID here

                        |<-- binlog reader has the GTID; -->|
                        |    a SELECT does not see the row  |
```

The plugin reports a GTID only after the engine commit, so it is never ahead of what a query can see.

## 2. What we build

One plugin with three parts. The server is not changed.

```
                               +------------------ plugin -------------------+
 Committing sessions           |                                             |          Client
 (clients, applier   --------> |  commit hook --> ring of GTIDs --> stream   | -------> (reader or
  workers)        after commit |      (a)             (b)           loop (c) |   push    proxy)
                               +---------------------------------------------+

 a: runs on the committing thread
 b: fixed size, in memory
 c: runs on the client's own connection thread
```

The hook only appends to the ring. The stream loop does all the sending.

## 3. Where the plugin learns that a GTID is committed

From the server's `after_commit` hook, the same interface semisync uses. The server calls it once per
transaction, in this order:

1. InnoDB commits the transaction. New queries now see its rows.
2. The server adds the GTID to `gtid_executed`.
3. The server calls `after_commit` and passes the transaction's GTID to the plugin.

This order holds for client sessions and for replication applier threads, single or parallel, and
with `binlog_order_commits` ON or OFF.

## 4. From the hook to the client, without slowing commits

The hook is on the commit path, so it does almost nothing: it appends the GTID to a ring and wakes
the stream loop.

- **The ring is bounded.** Entries are numbered. When it is full, the oldest entry is overwritten.
  The hook never waits and never does I/O.
- **When no client is connected, the hook returns at once** after one check.
- **The stream loop remembers the number of the last entry it sent.** It sends what is newer and
  sleeps when it has caught up.
- **A slow client** falls behind until its position is overwritten. The loop then sends the full set
  again and continues from the present. Commits are never delayed. A status counter records each
  such overrun.

## 5. How the client connects

The client starts a normal binlog dump, as any replica or binlog reader does, with one extra
variable. The server calls the plugin at the start of every dump, on that connection's thread. For a
connection that set the variable, the plugin keeps the thread and runs the stream loop on it. No
binlog data is sent on that connection.

```
 Client                                                        Server + plugin
   |                                                                 |
   |--- 1. connect and log in -------------------------------------->|
   |--- 2. SET @visible_gtid_stream = 1 ---------------------------->|
   |--- 3. start binlog dump --------------------------------------->|
   |                                                                 |  plugin keeps
   |<-- 4. full set of visible GTIDs --------------------------------|  this thread
   |<-- 5. one GTID event per committed transaction, as it commits --|
   |<-- ... ---------------------------------------------------------|
   |<-- 6. heartbeat when idle --------------------------------------|
   |<-- 7. full set again, every N seconds --------------------------|
   |--- 8. close the connection when done -------------------------->|
```

Steps 1 to 3 are standard. From step 4 on the server pushes; the client only reads.

- **Real replicas are not affected.** A connection without the variable is left alone and receives
  the binlog as always. Each connection has its own thread.
- **Security is the server's.** Login, TLS and the replication privilege apply as for any replica.
  There is no new port.
- **Reconnect.** If the connection drops or the server restarts, the client connects again. It needs
  no saved state because step 4 always sends the full set.

## 6. What the client receives

Ordinary binlog events, built by the plugin with the server's own event code. Any binlog client
library can decode them.

| Event | Meaning | Client does |
|-------|---------|-------------|
| Previous GTIDs | everything visible on this server right now | replace its set with this one |
| GTID | this transaction has just become visible | add it to the set |
| Heartbeat | server is alive, nothing new | nothing |

The client keeps a set and asks "is this GTID in the set?". There is no single "last GTID", because a
replica has GTIDs from several sources and a parallel applier can commit them out of numeric order.

This is the same client contract as in the core design. A client written for the plugin works
unchanged if the feature later moves into the server.

## 7. What the hook misses, and how the stream stays correct

In a few rare cases a GTID becomes part of `gtid_executed` without a call to the hook:

- a transaction that changed a non-transactional table and then rolled back;
- `SET GLOBAL gtid_purged`;
- a GTID committed with `sql_log_bin=0`.

The stream covers them with step 7: every N seconds the loop sends the full set again, read from
`gtid_executed`. A GTID the hook missed therefore appears after at most N seconds. The same step
repairs the client's set after a ring overrun.

`RESET BINARY LOGS AND GTIDS` has its own hook. The plugin sends the full set immediately, and the
client's set shrinks.

## 8. Limits

| Limit | Detail |
|-------|--------|
| Settings | `gtid_mode=ON`, binlog on, `log_replica_updates=ON` on replicas. The hook fires only for transactions written to the binlog. Otherwise the plugin refuses the stream with an error naming the setting. |
| Rare cases are late | The three cases in section 7 are reported up to N seconds late. Never early. |
| Installing other replication plugins | While a stream is connected, `INSTALL` or `UNINSTALL` of a replication plugin (for example semisync) waits. The plugin has a switch that disconnects all streams, to let such a statement proceed. Other replicas keep running meanwhile. |
| Plugin, not component | The commit hook is offered to plugins only. The plugin is built per server version, like semisync. |
| Cost of commit hooks | Once any plugin registers for commit hooks, the server calls them for every transaction. Semisync and Group Replication pay this today. To be measured. |

## 9. To confirm with a prototype

- The server accepts the dump request and reaches the plugin before sending anything, for both forms
  of the dump command.
- A stream loop that stays inside the hook reacts to client disconnect, `KILL`, shutdown and plugin
  uninstall.
- The GTID passed to the hook is always the transaction's own.
- Overhead of the hook on a busy server, with and without a client.

## Code references (8.4, c6c14fa65a0)

- Commit hook: sql/binlog.cc:8992 (ordered), :9146 (unordered); sql/rpl_handler.cc:806;
  sql/replication.h:120-150, :270, :761.
- Dump start hook, called on the connection's thread before anything is sent:
  sql/rpl_binlog_sender.cc:313.
- Precedent for a plugin reading a user variable and using the dump connection:
  plugin/semisync/semisync_source_plugin.cc:139-144, semisync_source_ack_receiver.cc:220.
- Hook list lock: sql/rpl_handler.cc:154-262 and the FOREACH_OBSERVER macro at :458. Hooks hold it
  shared; plugin install and uninstall take it exclusive.
- Rollback without a hook: sql/binlog.cc:3094-3098. Reset hook: sql/rpl_source.cc:1293.
- Reading `gtid_executed` from a plugin: include/mysql/group_replication_priv.h:143.
