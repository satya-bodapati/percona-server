# Backup DDL Journal — design

Status: DRAFT for review
Code: `storage/innobase/clone/clone0journal.{h,cc}` (built into InnoDB)
Consumer: Percona XtraBackup (`--lock-ddl=REDUCED --ddl-tracking=server`)
Related: PXB delta backup (`pxb-delta-backup-design.md`), MySQL clone.

---

## 1. Goal

### 1.1 The problem

A physical backup runs against a live server. While XtraBackup copies
tablespace files, the server keeps executing DDL — `CREATE`/`DROP`/`RENAME
TABLE`, `IMPORT TABLESPACE`, encryption changes, bulk index builds, undo
tablespace operations. To produce a consistent backup, XtraBackup must learn
about every such operation and reconcile it with the files it copied (drop a
file the server dropped, recopy one it created or rebuilt, follow a rename, and
so on).

Until now XtraBackup learned this by **copying and parsing the entire redo log**
for the whole backup window, decoding file-operation records
(`MLOG_FILE_CREATE`/`DELETE`/`RENAME`, `MLOG_INDEX_LOAD`, …). That approach has
lasting costs:

- The record-level parser must track every upstream redo-format change — a
  permanent maintenance liability across MySQL versions.
- It pulls encrypted-redo and keyring handling into backup time.
- It is hard to get right for operations that do **not** go through the
  redo-logged path (notably `IMPORT`, and undo DDLs).

### 1.2 The idea

The server already knows precisely when each of these operations happens — it
brackets every one of them with a notification for MySQL clone's own use. The
**backup DDL journal** hooks that existing notification point and records the
events to a small on-disk journal that XtraBackup reads directly. XtraBackup
then stops parsing redo for file operations entirely in this mode; it consumes
the journal instead.

### 1.3 Who uses it and how

XtraBackup, in `--lock-ddl=REDUCED --ddl-tracking=server` mode:

1. registers a backup session with the server before it starts copying files,
2. copies files while the server journals DDL events in the background,
3. under `LOCK INSTANCE FOR BACKUP`, asks the server to finalize (cut) the
   journal, reads it, and applies the resulting fixups (`.del`/`.ren`/`.new`,
   full recopies),
4. unregisters the session.

The result is the same consistent backup as before, without XtraBackup owning a
redo parser.

---

## 2. High-level design

### 2.1 What InnoDB already does for clone

InnoDB has, independent of this feature, an internal mechanism to be aware of
DDL as it happens. MySQL clone needs to coordinate with concurrent DDL (block
it, abort, or serialize around it), so InnoDB wraps every relevant tablespace
operation in a notification object, `Clone_notify`, and routes it through an
in-engine clone subsystem (`clone_sys`).

Two properties matter here:

- **It is in-engine, always on.** `clone_sys` is created during InnoDB startup,
  not by the loadable clone plugin. DDL notifications fire whether or not the
  clone plugin is installed and whether or not any `CLONE` statement is running.
  The backup journal therefore does **not** require clone to be set up in any
  way — it only requires a running InnoDB.
- **It already brackets each operation.** Because the notification is a scoped
  object, InnoDB is notified both when an operation *begins* and when it *ends*.
  (The exact begin/end mechanism is an integration detail covered in §4.)

The backup DDL journal is a thin sink attached to this existing notification
path. It adds no new call sites in the DDL code and imposes no dependency on
clone being active.

### 2.2 What DDL events are tracked

The server notifies for the following categories (full mapping in §4.3):

- tablespace **create**, **drop**, **rename**;
- **import** (DISCARD/IMPORT), which brings a file in outside the redo-logged
  create path;
- **encryption** change (per-table and general tablespace);
- **bulk index build** (in-place `ADD INDEX`);
- **undo tablespace** DDL (create/drop/truncate);
- **redo logging disabled** (`ALTER INSTANCE DISABLE INNODB REDO_LOG`) — a hard
  stop for any physical backup.

Plain in-place ALTER that only touches metadata is notified too, but has no file
impact and is ignored by the consumer.

---

## 3. Component design

### 3.1 Job

Capture, per backup session, the stream of DDL notifications that occur during
that session's window and expose it to an external backup tool as a
self-contained, readable file. The component owns only recording and session
bookkeeping; it makes no backup decisions itself — reconciliation is the
consumer's job.

### 3.2 What it registers with, and is called by

- **Called by InnoDB.** InnoDB's `Clone_notify` hook invokes the component's
  `log(type, space, begin)` entry point on every DDL begin and end (§4). A
  second entry point, `log_point(type, space)`, records a self-contained event
  where there is no scoped notifier to bracket it (see §3.6, import).
- **Registers UDFs for the client.** At InnoDB startup the component registers
  three SQL functions through `srv::Dynamic_procedures` (the same mechanism used
  by `innodb_redo_log_consumer_*`), under the module name
  `innodb_backup_ddl_journal`. These are the client's control surface.

### 3.3 Interfaces and their purpose

The client (XtraBackup) drives a session through three UDFs:

- `innodb_backup_ddl_journal_start()` → `backup_id`
  Register a session. Creates the session's on-disk file, writes its header,
  and turns on event capture. Returns a unique `backup_id` (0 on error). Called
  **before** file copy begins, so no DDL in the window is missed.

- `innodb_backup_ddl_journal_cut(backup_id)` → slice path
  Finalize the session's file and return its path (`NULL` on error). By the time
  the client calls this it holds `LOCK INSTANCE FOR BACKUP`, so no further DDL
  can occur; the file already contains every event of the window. Cut simply
  makes it durable (`fsync`) and hands back the path to read.

- `innodb_backup_ddl_journal_stop(backup_id)` → 0/1
  Unregister the session and delete its file. Called after the client has
  consumed the journal.

Internally, `log()` / `log_point()` are the ingest side (InnoDB → component);
they are not exposed to SQL.

### 3.4 Files it creates, and how they are read

Each session owns one append-only file:

```
<datadir>/#ib_backup_tracking/ddl_journal.<backup_id>
```

The directory is created on first session start. The file is **JSONL** — one
JSON object per line. The first line is a header; the rest are events:

```json
{"event":"header","version":2,"backup_id":42,"start_lsn":193841}
{"seq":1,"ev":"BEGIN","type":"SPACE_RENAME","space":9,"flags":16417,"lsn":193850,"path":"./test/t1.ibd"}
{"seq":2,"ev":"END",  "type":"SPACE_RENAME","space":9,"flags":16417,"lsn":193920,"path":"./test/t2.ibd"}
```

The reader (XtraBackup) reads line by line and parses each with rapidjson (the
component also *writes* each line with rapidjson, so escaping is symmetric):

- The **header** is recognized by the presence of an `event` member and its
  fields are ignored by the reader; it exists for versioning and debugging.
- **Event** lines carry `seq, ev, type, space, lsn, path` and an optional
  `flags` (defaults to 0 if absent, for forward compatibility).
- Consumers rely on **ordering and presence**, never on exact `lsn` values (redo
  granularity is coarser than a single record). `path` is empty when the space
  does not exist at that instant (e.g. a CREATE's BEGIN, a DROP's END) — see §4.

### 3.5 Concurrent sessions

More than one backup may be registered at once. The component uses a
**file-per-session, fan-out-on-write** model:

- State is a map `backup_id → {fd, seq, bytes, broken}`. Each session has its
  own file descriptor and its own sequence counter.
- On each DDL event, `log()` — under a single mutex — iterates every registered
  session and appends the event to **each** session's file. One DDL therefore
  writes once per open session.
- "Which events are mine?" is answered by physical separation: a file only ever
  receives lines written while its session was registered. Two sessions started
  at different times naturally contain different, correctly-overlapping event
  sets, with no shared log and no cross-session byte offsets.

This keeps sessions fully independent: a write fault on one session's file sets
only that session's `broken` flag (its `cut` then fails and its backup aborts);
other sessions are untouched. Cost is `N` writes per DDL for `N` concurrent
sessions — negligible in practice (see §6 for the note on this).

**Write-fault safety.** Records are all-or-nothing. The writer advances a
session's confirmed size only by the bytes actually written; on a short/failed
write (e.g. ENOSPC) it truncates the partial tail back to the last whole line
and marks the session `broken`. A session's file therefore never contains a torn
JSON line, and an incomplete journal reliably fails the backup rather than being
silently trusted.

### 3.6 End-to-end workflow (typical client: XtraBackup)

```
XtraBackup                                    Server (component)
──────────                                    ──────────────────
start()  ───────────────────────────────────► create slice, write header,
   ◄── backup_id                                 enable capture

[copy tablespace files]                        (concurrent DDLs fan out to the
                                                slice as BEGIN/END lines)

LOCK INSTANCE FOR BACKUP
cut(backup_id) ─────────────────────────────► fsync slice
   ◄── slice path
[read slice, build fixup maps:
 .del / .ren / .new, full recopies]

stop(backup_id) ────────────────────────────► close + delete slice
UNLOCK INSTANCE
```

Reconciliation logic (what the fixups mean, prepare-side handling of imports,
etc.) lives in XtraBackup and is described in the delta backup design doc; the
component's contract ends at "here is the ordered, complete event file for your
session."

---

## 4. InnoDB integration: the `Clone_notify` hook

This section is the mechanism by which §3's `log()` is actually called. It is
placed after the component so the reader already knows what `log()` feeds.

### 4.1 One scoped object → a begin and an end

`Clone_notify` is a stack object InnoDB constructs at the start of a DDL and
destroys at its end. A DDL site writes a single line, e.g. in `fil0fil.cc`:

```cpp
Clone_notify notifier(Clone_notify::Type::SPACE_CREATE, space_id, false);
//                                                                 ^ no_wait, not begin/end
auto space = fil_space_create(...);   // the operation's actual work
// ... redo-log the create ...
// `notifier` destroyed at scope exit
```

That one object yields **two** journal writes via RAII:

- its **constructor** calls `log(type, space, begin=true)` → a **BEGIN** line,
  emitted before the operation's effect is visible;
- its **destructor** calls `log(type, space, begin=false)` → an **END** line,
  emitted after the operation completed.

The DDL site never chooses begin/end and never calls the journal twice; the pair
falls out of the object's lifetime. The constructor's third argument (`no_wait`)
controls clone blocking and is unrelated to journaling.

### 4.2 Why both a BEGIN and an END line

A single line captures the tablespace's state at one instant, but the *useful*
state is at a different instant depending on the operation, because `path`/
`flags` are resolved when the line is written:

| Operation | Meaningful state | Line the consumer uses |
|-----------|------------------|------------------------|
| CREATE    | after (space now exists) | END `path`/`flags` |
| DROP      | before (file still exists) | BEGIN `path` (END path is empty) |
| RENAME    | both (old → new) | BEGIN `path` + END `path` |
| IMPORT / UNDO_DDL | both | BEGIN and/or END `path` |

So BEGIN/END is a **before-image / after-image pair**, not an ad-hoc marker
scheme; the consumer takes whichever end carries the state it needs. A one-line
alternative would force the server to snapshot the before-state at the
constructor, hold it in the object, and emit a combined record at the
destructor — more server-side state and logic for no consumer benefit. A
dangling BEGIN (no END) additionally flags an in-flight operation, which cannot
survive once `LOCK INSTANCE FOR BACKUP` has returned.

### 4.3 Notification type → event → consumer action

All types are journaled as a BEGIN/END pair. "PXB action" is what the consumer
does on the END line.

| `Clone_notify::Type` | Server call site(s) | Trigger | PXB action |
|----------------------|---------------------|---------|------------|
| `SPACE_CREATE` | `fil0fil.cc` (`fil_ibd_create`); `ha_innodb.cc` (synthetic, post-IMPORT) | create table / general tablespace / imported file | new table; if a drop of the same id precedes it → recreate/reimport, recopy in full |
| `SPACE_DROP` | `fil0fil.cc` (file delete) | drop / DISCARD | mark dropped (`.del`), using BEGIN path |
| `SPACE_RENAME` | `fil0fil.cc` | rename | rename (`.ren`): old = BEGIN path, new = END path |
| `SPACE_IMPORT` | `ha_innodb.cc` (`s_invalid_space_id`) | DISCARD / IMPORT | generic signal only — real work via synthetic `SPACE_CREATE` + DISCARD's `SPACE_DROP` (§4.4) |
| `SPACE_ALTER_ENCRYPT`, `…_GENERAL`, `…_GENERAL_FLAGS` | `srv0srv.cc`, `fsp0fsp.cc` | encryption change | recopy in full ("encryption") |
| `SPACE_ALTER_INPLACE` | `handler0alter.cc` | in-place ALTER (metadata) | none — no file impact |
| `SPACE_ALTER_INPLACE_BULK` | `ddl0builder.cc`, `ddl0ctx.cc` | bulk index build | recopy in full ("bulk index load") |
| `SPACE_UNDO_DDL` | `trx0purge.cc`, `ha_innodb.cc` (`s_invalid_space_id`) | undo create/drop/truncate | drop-if-before / create-if-after; complemented by PXB's own before/after undo scan |
| `SYSTEM_REDO_DISABLE` | `mtr0mtr.cc` | disable redo logging | **fatal** — backup inconsistent, abort |

### 4.4 IMPORT is the exception that needs a synthetic event

`IMPORT` brings a file in physically, outside the redo-logged create path, and
assigns a possibly-new space id. The stock `SPACE_IMPORT` notifier is
constructed with `s_invalid_space_id`, so on its own it cannot tell the backup
which file to recopy. Correctness comes from two other events:

1. **DISCARD** deletes the old file → `SPACE_DROP` for the old id → PXB marks it
   dropped.
2. After `row_import_for_mysql` succeeds, `ha_innodb.cc` emits a **synthetic
   `SPACE_CREATE`** via `log_point()` carrying the real final id, path, and
   authoritative flags → PXB recopies that file in full.

`log_point(type, space)` is one BEGIN immediately followed by one END, used
wherever there is no `Clone_notify` scope to produce the pair (the import hook is
its only user today). It keeps a single record shape so the consumer has one code
path. The same-id and new-id import cases, and the prepare-side `.reimport`
handling, are described in the delta backup design doc.

---

## 5. User interface / deliverables

- **No new loadable component (`.so`).** The journal is compiled into InnoDB
  (`libinnobase`); `backup_ddl_journal_init()` runs during log-subsystem startup
  and registers the UDFs automatically. There is no `INSTALL COMPONENT` step.
- **No new server option or system variable.** Capture is dormant until a client
  calls `innodb_backup_ddl_journal_start()`, and there is nothing to configure.
- **New SQL surface:** three internal UDFs — `innodb_backup_ddl_journal_start`,
  `innodb_backup_ddl_journal_cut`, `innodb_backup_ddl_journal_stop`.
- **New on-disk layout:** a `<datadir>/#ib_backup_tracking/` directory holding
  `ddl_journal.<backup_id>` slice files. Files are created per session and
  removed on stop; the directory is otherwise empty when no backup runs.
- **Client-side option (XtraBackup, not the server):**
  `--ddl-tracking=auto|redo|server`. `auto` uses the server journal when the
  server provides these UDFs and falls back to the legacy redo parser otherwise;
  `server` requires them; `redo` forces the parser.

---

## 6. Known limitations and future improvements

- **Concurrent backups fan out writes.** `N` simultaneous sessions cost `N`
  writes per DDL. Running multiple component backups at once is uncommon, so
  this is accepted for v1. If it ever matters, revisit a shared log with
  per-session views — at the cost of reintroducing offset bookkeeping, so only
  if measurements justify it.
- **Write-fault path is not yet unit-tested.** Exercising the short-write /
  `broken`-slice branch in MTR needs a debug-injection hook in the component;
  tracked as a follow-up.

---

## 7. Testing

- **MTR `percona.backup_ddl_journal`:** session lifecycle; capture of
  create/rename/bulk/drop; header present and one-JSON-object-per-line;
  unknown-session error handling; two concurrent sessions get independent,
  correctly-overlapping slice files.
- **XtraBackup `deltabackup` suite (integration, server with the component):**
  `component_ddl_redo_copy`, `import_ddl`, `import_incremental`, and the full
  delta / encryption / undo / rename / lock-ddl matrix.
