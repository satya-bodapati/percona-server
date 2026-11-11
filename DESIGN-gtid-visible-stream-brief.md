# Visible GTID Stream

Short design. Percona Server 8.4. Proposal, not implemented.

The server tells a proxy which GTIDs a query can already see, so reads that follow a write can go to
a replica safely.

## 1. The problem we are solving

An application writes a row through ProxySQL and reads it back straight away. The write goes to the
primary; the read may go to a replica. If the replica has not applied the write yet, the application
does not see its own row.

ProxySQL avoids this with GTIDs:

- The primary returns the GTID of each commit to ProxySQL in the OK packet.
- ProxySQL keeps, per replica, the set of GTIDs that replica has. A read goes only to a replica whose
  set contains the client's last GTID.

To keep those sets current, a helper process (`proxysql_binlog_reader`) reads each replica's binlog
and reports every GTID it sees.

## 2. What goes wrong today

A transaction is written to the replica's binlog *before* it is committed in InnoDB. The reader
reports the GTID when it appears in the binlog. For a short time ProxySQL believes the replica has
the write, sends the read there, and the row is not visible yet.

```
One commit group, time runs left to right

 | 1 Flush              | 2 Sync         | 3 Commit            | done
 | write to binlog file | fsync binlog   | commit in InnoDB    |
                        ^                                      ^
                        |                                      |
          flushed / synced position                  committed position
          (binlog_end_pos: readers                   (a query can see the row)
           see the GTID)

                        |<-- GTID already reported; a SELECT -->|
                        |    does not see the row               |
```

The server tracks only the first position today. This design adds the second one and streams from it.

The two positions differ on purpose. The binlog must be written first because it decides, after a
crash, which transactions are committed. Replication, and semisync in particular, must be able to
send a transaction before it is committed. So the existing position cannot simply be moved later.

The gap is usually under a few milliseconds, but it grows without limit when the fsync or a semisync
acknowledgement is slow.

## 3. Where the server knows what is committed

In the last stage of group commit. One thread, the group leader, does three things in order for the
whole group:

1. **InnoDB commits each transaction.** InnoDB removes the transaction from its list of active
   transactions. From that moment every new query sees its rows.
2. **The server adds the group's GTIDs to `gtid_executed`.** This is the in-memory set behind
   `@@global.gtid_executed`.
3. **New: the leader records the binlog position where the group ends.** We call it the *last
   committed position*. Everything in the binlog before it is committed.

Every transaction that has a GTID in the binlog passes through this stage: client transactions,
replication applier threads (single or parallel), DDL, and the empty transactions a filtered replica
writes. With `binlog_order_commits=ON`, groups commit in the order they were written, so one position
is enough to describe what is committed.

InnoDB itself is not changed. It does not need to know about the position; the leader records it
right after InnoDB has finished.

## 4. Sending it to many clients without slowing commits

The committing side does one small thing per group: store the new position and wake any waiting
stream threads. It never builds a list, never copies GTIDs, and never waits for a client.

Each client gets its own server thread. That thread reads the binlog file, which already contains
every GTID, up to the last committed position, and sends only the GTID events. When it reaches the
position it sleeps until the leader moves it.

```
replica binlog file

 | T1 | T2 | T3 | T4 | T5 | T6 | T7 |   not written yet
 |<------ committed ----->|<-written->|
                          ^           ^
            last committed position   binlog_end_pos
                  (new)

 client A: up to date, waiting at the position        ------------------>|
 client B: slow, still at T3; holds nothing in memory -------->|
```

T1-T5 are committed. T6-T7 are written but not yet visible. A slow client is just further back in the
file. It delays nobody.

There is no queue between committers and clients, so there is nothing to fill up, nothing to trim,
and no per-client state beyond a file offset.

## 5. Same mechanism as `binlog_end_pos`

Replication already works exactly this way. The leader stores `binlog_end_pos` under a small
dedicated mutex and signals; each replica's dump thread reads the file up to it and waits there. We
add a second position of the same kind and reuse the dump thread code.

|          | Existing | New |
|----------|----------|-----|
| Position | `binlog_end_pos`: flushed or synced | last committed position |
| Moved    | after flush or sync | after the InnoDB commits |
| Mutex    | `LOCK_binlog_end_pos` | `LOCK_binlog_last_committed_pos` |
| Reader   | dump thread, sends whole events | dump thread in "visible" mode, sends GTID events only |

Existing replication, semisync and `binlog_end_pos` are not changed.

## 6. How a client connects and reads

```
 Client (reader / proxy)                               Server (replica)
        |                                                    |
        |--- 1. connect and log in, as a replica does ------>|
        |--- 2. SET @visible_gtid_stream = 1 --------------->|
        |--- 3. start binlog dump (COM_BINLOG_DUMP_GTID) --->|
        |                                                    |  starts at the last
        |<-- 4. full set of visible GTIDs (once) ------------|  committed position
        |<-- 5. one GTID event per newly committed trx ------|
        |<-- ... --------------------------------------------|  waits when it
        |<-- ... --------------------------------------------|  reaches it
        |<-- 6. heartbeat when nothing happens --------------|
        |--- 7. close the connection when done ------------->|
```

Steps 1 and 3 are what any replica or binlog reader does today. Step 2 is the only new request. Any
number of clients can do this at the same time.

If the connection drops, or the server restarts or resets its GTIDs, the client connects again and
starts from step 1. It needs no saved state, because step 4 always gives the full set.

## 7. How the client understands the stream

The stream uses ordinary binlog events, so any existing binlog library can decode it. The client sees
three kinds that matter:

| Event | Meaning | Client does |
|-------|---------|-------------|
| Previous GTIDs | everything visible on this server right now | replace its set with this one |
| GTID | this transaction has just become visible | add it to the set |
| Heartbeat | server is alive, nothing new | nothing |

**The client keeps a set, not one "last GTID".** A replica can have GTIDs from several sources, and a
parallel applier can commit number 12 before number 11. So there is no single last value. The
question ProxySQL asks is "is the client's GTID in this replica's set?", which is the check it
already does today.

While a client is disconnected, or before it has received the first Previous GTIDs event, it treats
the replica's set as unknown and does not send causal reads there.

## Settings this needs

`gtid_mode=ON`, binlog on, `log_replica_updates=ON` on replicas, `binlog_order_commits=ON`. All are
defaults in 8.4. If one is off, the server refuses the stream with an error naming it.

Details, code references and open questions are in the full design:
`DESIGN-gtid-visible-stream.md`.
