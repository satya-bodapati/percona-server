# Visible GTID Stream as a plugin

Alternative design. No change to the core server. Proposal, not implemented; nothing was built or
measured. Names are proposals.

The goal is the same as in `DESIGN-gtid-visible-stream-brief.md`: tell a proxy which GTIDs a query on
this server can already see. Here the source is the `after_commit` hook and everything lives in a
plugin that can be installed on upstream MySQL and on Percona Server.

## 1. Constraints

- **No core change.** Only interfaces the server already offers to plugins.
- **Plugin, not component.** The commit hooks are a plugin-only interface
  (`register_trans_observer`, sql/replication.h:761). No component service exposes them. Semisync
  and Group Replication use the same interface.
- **Built per server version.** Like semisync, the plugin is compiled against the server's headers.
  One source tree, one build per MySQL / Percona Server version.
- **Required settings:** `gtid_mode=ON`, binlog on, `log_replica_updates=ON` on replicas. The hook
  only fires for transactions written to the binlog. `binlog_order_commits` may be ON or OFF.

## 2. The pieces

```
 committing sessions                    plugin                                client
 (clients, applier workers)

  engine commit                                                        SELECT visible_gtid_snapshot()
  gtid_executed updated                                                SELECT visible_gtid_next(cursor, timeout)
        |                                                                        |
        v                                                                        v
  after_commit hook  ---- GTID ---->  [ bounded ring of GTIDs ]  <---- reads from its cursor,
  (plugin callback)                     numbered 1, 2, 3, ...          sleeps when it has caught up
```

Three parts:

1. **A hook callback** that puts each committed GTID into a ring.
2. **A bounded ring** in memory. Entries are numbered. Old entries are overwritten.
3. **Two SQL functions** a client calls over a normal MySQL connection to read from the ring.

There is no new port, no new protocol and no new thread. The client's own connection thread does the
reading and the waiting.

## 3. The source: the `after_commit` hook

The server calls the hook once per transaction, after the engine commit and after `gtid_executed` is
updated (sql/binlog.cc:8992 with ordered commits, :9146 without). So a GTID seen in the hook is
already visible to queries. This is the safe side.

The hook receives the transaction's GTID, including the UUID and tag (`Trans_param.gtid_info`,
sql/replication.h:120-150).

The callback does the minimum:

```
if no client has asked for data recently:  return        // one atomic read
lock ring;  ring[next++] = gtid;  unlock ring
wake waiting readers, if any
```

It never does I/O and never waits for a reader. When the ring is full it overwrites the oldest entry.

The ring has its own short mutex because, with `binlog_order_commits=OFF`, many sessions call the
hook at the same time. With ordered commits only the group leader calls it.

## 4. How a client reads

The client holds a **cursor**: the number of the last ring entry it has seen, plus an id of this
plugin instance. The server keeps no state per client.

```
 Client                                                     Server (plugin)
   |                                                              |
   |-- 1. connect and log in (normal MySQL connection) ---------->|
   |-- 2. SELECT visible_gtid_snapshot() ------------------------>|  note ring position,
   |<----- cursor + full set of visible GTIDs --------------------|  then copy gtid_executed
   |                                                              |
   |-- 3. SELECT visible_gtid_next(cursor, timeout) ------------->|  entries after cursor?
   |                                                              |  if none: sleep until the
   |<----- new cursor + GTIDs committed since --------------------|  hook adds one, or timeout
   |                                                              |
   |-- 3. again, with the new cursor ---------------------------->|
   |<----- ... ---------------------------------------------------|
   |                                                              |
   |<----- "resync" (cursor too old, or instance changed) --------|
   |-- 2. again ------------------------------------------------->|
```

- **Step 2 gives the full set.** The plugin first notes the ring position, then copies
  `gtid_executed`. A GTID committed in between is in the set and also comes again from the ring.
  Adding a GTID twice is harmless.
- **Step 3 is a blocking call, not polling.** While the client waits inside the call, the server
  answers the moment a transaction commits. If commits happen while the client is between two calls,
  they wait in the ring and the next call returns them at once, as one batch.
- **Timeout** (for example one second) returns an empty answer. It works as a heartbeat and lets the
  server notice a client that has gone away.
- **"resync"** is returned when the client's cursor has been overwritten in the ring, or the plugin
  was restarted. The client goes back to step 2.
- **Finishing:** the client stops calling and closes the connection. There is nothing to unregister.

### What the client parses

Plain text in the standard GTID notation, which every MySQL client already handles. No binlog
parsing.

| Call | Returns | Client does |
|------|---------|-------------|
| `visible_gtid_snapshot()` | `cursor=<id>:<n>;set=<uuid>:1-500,<uuid2>:1-90` | replace its set |
| `visible_gtid_next(cursor, timeout)` | `cursor=<id>:<n>;gtids=<uuid>:501,<uuid>:503` | add each to its set |
| | `cursor=<id>:<n>;gtids=` | nothing (timeout) |
| | `resync` | call the snapshot function |

The client keeps a set, as in the core design. It routes a causal read to this server only if the set
is known and contains the GTID.

## 5. Why SQL functions and not a network stream

A plugin has three ways to send data to a client. Only one needs no new infrastructure.

| Way | Precedent | Verdict |
|-----|-----------|---------|
| Blocking SQL functions on a normal connection | Group Replication and others register functions | **Chosen.** Authentication, TLS, privileges and KILL come for free. Works from any client language. |
| The plugin opens its own port | X Plugin, Group Replication | Possible later. The plugin must then provide its own authentication and TLS. |
| A protocol command handed to the plugin | Clone plugin | Not available. Clone has its own command, `COM_CLONE`, built into the core (sql/sql_parse.cc:2008). The clone plugin does not open a port. A new command is a core change. |

The replication dump command cannot be extended by a plugin either; that code is in the core.

For ProxySQL the SQL functions are also the easier client: it already speaks the MySQL protocol, and
it would not need a binlog parser. The separate reader process is no longer needed.

## 6. Cases the hook misses

The hook is not called in these cases, although the GTID becomes part of `gtid_executed`:

| Case | How often | Handled by |
|------|-----------|------------|
| Transaction written to the binlog that ends in ROLLBACK (it changed a non-transactional table). sql/binlog.cc:3094-3098: the hook runs only for XA ROLLBACK. | rare | periodic resync |
| `SET GLOBAL gtid_purged` | rare, admin action | periodic resync |
| GTID committed with `sql_log_bin=0` | rare | periodic resync |
| `RESET BINARY LOGS AND GTIDS` | rare, admin action | `after_reset_master` hook (sql/rpl_source.cc:1293): the plugin changes its instance id, all clients resync |
| Plugin installed while the server runs; server restart | - | every client starts with a snapshot |
| Binlog off or `log_replica_updates=OFF` | by configuration | not supported; the functions return an error naming the setting |

**Periodic resync** is the safety net. Every N seconds (configurable; 0 turns it off) the next call
returns "resync", and the client fetches the full set again. This reuses the connect path, so it
needs no extra code. A GTID missed by the hook therefore appears after at most N seconds instead of
never. The cost is one copy of `gtid_executed` per client every N seconds.

This is the one place where the plugin design is weaker than the core design: for these rare cases
it is late by up to N seconds, where the core design has no gap.

## 7. Cost and slow readers

- **Hook:** one atomic read when no client is active. With a client: one short mutex, one small
  copy, per transaction. With ordered commits the group leader does this for the whole group.
- **Having any commit observer installed has a cost of its own.** The server calls all transaction
  hooks (begin, before commit, after commit) for every transaction once a plugin is registered.
  Semisync and Group Replication pay this today. It must be measured.
- **Memory:** the ring, fixed size, for example 64k entries.
- **Slow reader:** falls behind, its cursor is overwritten, it gets "resync" and continues from a
  fresh snapshot. It never slows commits. A status counter records each overrun, so we can see
  whether the ring size is right before tuning anything.
- **Snapshot:** copies `gtid_executed`, which briefly takes the GTID lock. Done at connect and at
  each resync, per client. Upstream already offers a function for this to plugins
  (`get_server_encoded_gtid_executed`, include/mysql/group_replication_priv.h:143).

Status variables: active clients, GTIDs added to the ring, ring overruns, snapshots served.

## 8. Compared with the core design

|  | Core (last committed position) | Plugin (after_commit hook) |
|--|--------------------------------|----------------------------|
| Server change | small patch in the binlog code | none |
| Installable on upstream MySQL | no | yes |
| Complete | yes, by construction | yes, but rare cases are late by up to N seconds |
| Queue | none; clients read the binlog | bounded ring |
| Slow reader | further back in the file | resync from a snapshot |
| `binlog_order_commits=OFF` | not supported | supported |
| Binlog off | not supported | not supported |
| Client | binlog reader library, one new option | plain SQL, two function calls |
| Transport | replication protocol | normal MySQL connection |
| Cost on commit | one store per commit group | one ring append per transaction, plus the general cost of commit observers |

## 9. To verify before implementation

- That the GTID passed to the hook is always the transaction's own. The server takes it from a
  "last used GTID" field of the session (sql/rpl_handler.cc:806-820).
- The cost of having a commit observer installed, on a busy server.
- That a function blocked in a wait reacts to KILL, to shutdown and to plugin uninstall.
- That `get_server_encoded_gtid_executed` is usable outside Group Replication on all target versions.
- How long function results may be, for a very fragmented GTID set.

## 10. Decisions

1. Core design, plugin design, or plugin first as a quick path and core later.
2. Resync period N, and whether "late by up to N seconds in rare cases" is acceptable.
3. Privilege needed to call the functions.
4. Whether a later version should also offer its own port speaking ProxySQL's existing GTID
   protocol, so that ProxySQL needs no change at all.
