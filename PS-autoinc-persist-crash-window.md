# Persistent-counter crash window in dict_table_autoinc_log (and vec twin)

Status: NOT fixed anywhere (deliberate; 2026-07-17). File as a PS ticket with
the MTR below. Affects upstream MySQL too (WL#7806 machinery). The Percona
vector counter (`dict_table_vec_next_id_log`, PS-11300) currently mirrors the
upstream shape and shares the window; its fix is trivial (see §Fix) but was
deferred to keep parity until the upstream ticket is decided.

## The bug

Both counter-persistence functions advance the in-memory "persisted watermark"
BEFORE the covering redo record is even placed into a mini-transaction:

- `dict_table_autoinc_log` (dict0dict.cc): `mutex_enter` → advance
  `autoinc_persisted` → `mutex_exit` → **then** `persister->write_log(mtr)`.
  The per-table mutex is already released when the window opens, so it does
  not help. Upstream shrinks the window by logging first-thing into the row's
  own mtr ("Always log the counter change first", row0ins.cc:~2501) — but the
  skip-race below is unaffected.
- `dict_table_vec_next_id_log`: CAS-max watermark → mark dirty →
  `write_log` → own `mtr.commit()`. Same shape, same window.

A second thread that observes the advanced watermark with a smaller value
SKIPS writing any redo ("already covered") — trusting a value that exists
nowhere durable, nor even in any mtr buffer yet.

## Timeline of doom (counter durable at 9)

| t | Thread B (bigger value, 20) | Thread A (smaller value, 15) |
|---|---|---|
| 1 | advances watermark 9 → 20 | |
| 2 | **preempted before write_log** — record "20" exists nowhere | |
| 3 | | sees watermark 20 ≥ 15 → skips logging |
| 4 | | inserts + COMMITs → row durable; flushed log has NO counter record |
| 5 | **crash** (B never resumed) | |

Recovery restores counter = 9. A's committed row with value 15 exists →
counter reissues 10..15 → duplicate of a COMMITTED value (dup key on autoinc
PK; for vec: duplicate aux label of a committed row).

## MTR repro sketch (needs two new DEBUG_SYNC points)

### autoinc variant
Add in `dict_table_autoinc_log` between `mutex_exit` and `write_log`:
`DEBUG_SYNC(current_thd, "autoinc_wm_advanced_before_log");`
(guard current_thd != nullptr; background threads skip).

```
CREATE TABLE t (id BIGINT AUTO_INCREMENT PRIMARY KEY, v INT) ENGINE=InnoDB;
INSERT INTO t VALUES (9, 0);                     # counter/watermark = 9ish
--source include/restart_mysqld.inc              # clean persisted baseline
connect(con1); SET DEBUG_SYNC='autoinc_wm_advanced_before_log SIGNAL wm_up WAIT_FOR go';
--send INSERT INTO t VALUES (20, 0)              # explicit big value -> advances wm, stalls pre-log
connection default; SET DEBUG_SYNC='now WAIT_FOR wm_up';
INSERT INTO t VALUES (15, 0);                    # 15 < wm 20 -> SKIPS logging; commits durable row
--source include/kill_and_restart_mysqld.inc     # con1 never wrote redo
SELECT AUTO_INCREMENT FROM information_schema.tables WHERE table_name='t';
# BUG: shows 10 (recovered from 9), not >= 16/21.
INSERT INTO t (v) VALUES (1),(1),(1),(1),(1),(1);  # walks into id 15
# -> ER_DUP_ENTRY on a committed row = the corruption made visible
```

### vec variant (branch ps-11300-hnsw-populate)
Two points needed because vec values are assigned monotonically:
- P1 `vec_id_assigned_before_persist` in `vec_assign_next_idx_id` between
  fetch_add and the log call (lets thread A hold its small id while B runs).
- P2 `vec_wm_advanced_before_log` in `dict_table_vec_next_id_log` between the
  CAS and `write_log`.
conA: insert, stall at P1 holding id N. conB: insert → id N+1 → stall at P2
(watermark N+1, no redo). conA: resume → N < N+1 → skips → commit.
kill -9. After restart, insert → gets id N (duplicate of conA's committed aux
label → aux dup key / vec_aux_dump shows the collision).

## Fix (for the ticket discussion)

Reorder to close the window completely: `write_log` + make the record's mtr
commit BEFORE advancing the watermark. Then "watermark >= my value" implies a
covering record is already ordered in the log at a lower LSN than anything
the skipper subsequently commits — sequential log flush makes under-recovery
impossible. Racers may both log; recovery aggregates max (harmless).
- Trivial for the vec counter: it uses a dedicated mtr
  (`vec_assign_next_idx_id`), so commit-then-CAS is a local reorder.
- Harder for autoinc: the record rides the row's mtr (crash-atomicity with
  the row), so "commit first" isn't free; option: hold the watermark advance
  until mtr commit via mtr commit hooks, or accept skip-side logging
  (skip only if watermark >= value AND covering LSN <= last durable LSN).

## Notes
- Probability: window is a few instructions wide, needs preemption exactly
  there + racing smaller value + crash before resume. Never reported in the
  field. Still a correctness hole in a mechanism whose only job is "never
  reissue".
- Eviction/reload/flusher interplay was audited and is NOT part of the bug:
  eviction requires ref_count==0 and flushes dirty metadata first; the
  flusher coordinates via dict_persist->mutex + dirty_status only.
