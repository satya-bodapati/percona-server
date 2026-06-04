# Handoff: PS-11175 natural-workload repro

You are picking up an in-flight session from another machine. The user
is **Satya Bodapati** at Percona, working on PS-11175 (page-tracking
2-byte truncation in `mach_write_to_2` inside `Arch_Block::add_reset`).

This box is faster than the one the session was on. The previous run
was disk-bound at ~2,600 cleaner page-flushes/sec — `ib_page_33` would
have taken ~35 minutes to fill on that hardware. The goal here is to
run the test on this beefier machine and see whether the cleaner can
actually keep up.

## The bug (one-paragraph)

`Arch_Block::add_reset` persists `reset_pos.m_block_num` (8-byte
ulint) via `mach_write_to_2`, which silently truncates to 16 bits.
Once the page-archiver's `m_block_num` crosses 65,535 (after roughly
4.2M page-id entries), every saved reset entry is mangled. Recovery
later dereferences the truncated value to either (a) a file index
that doesn't exist on disk → ENOENT → `srv_fatal_error` MY-012646
[face B], or (b) a slot offset past the partial-active file's EOF
→ 32-byte read past EOF → MY-012642 → recovery can't start [face A].

Customer-visible: backup software's `mysqlbackup_page_track_get_changed_pages`
crashes the server post-restart; subsequent restarts can fail too.

## Branch

- Branch: `ps_11175_natural_repro` on `satya-bodapati/percona-server`
- 4 commits on top of `origin/8.0` (mysql/percona-server)
- URL: https://github.com/satya-bodapati/percona-server/tree/ps_11175_natural_repro

```
5532f042163  PS-11175 [8.0]: shell-driven sysbench repro (alternative to MTR)
ab06153097b  PS-11175 [8.0]: add innodb_limit_leaf_optimistic_insert_debug + DBUG bypass
8eacf128e5e  PS-11175 [8.0]: natural-workload MTR repro for both bug faces
8e6ce20023c  PS-11175 [8.0]: seed knob + diagnostic prints for natural repro
```

## What's in those commits

**Source (5 files in `storage/innobase/`):**
- `innodb_arch_page_initial_block_num` sysvar — seeds the writer's
  block counter so a short workload can cross block 65535 without
  needing hours of natural workload. Test sets it to **67419** (first
  slot of `ib_page_33`).
- `innodb_limit_leaf_optimistic_insert_debug` — cherry-picked debug-only
  knob that caps records-per-leaf, forcing splits per few inserts to
  multiply the dirty-page rate. Test sets it to **8**.
- 4 diagnostic prints, all gated by `srv_arch_page_initial_block_num != 0`
  so production stays silent:
   - `Arch_File_Ctx::open_new`: new `ib_page_N` file created
   - `Arch_Page_Sys::save_reset_point`: every `set(1)` — shows real
     block_num + the truncated 2-byte value + which file/slot recovery
     will dereference
   - `track_page` block-roll: writer m_block_num advance, throttled
   - `Arch_Block::flush` data-block path: full + partial flushes
     to disk, throttled to every 16th
- `DBUG_EXECUTE_IF("disable_mach_2_write_assertion")` in
  `mach_write_to_2` — bypasses the debug-only `ut_ad((n|0xFFFF)<=0xFFFF)`
  so the truncation can be observed in debug builds. Test sets the flag.
- `Arch_Group::write_to_file` assertion `m_file_ctx.get_count() == 0`
  relaxed to also allow the seeded case (where `m_count == file_index`
  at first write).

**MTR test (`mysql-test/suite/percona/t/ps_11175_natural_repro.test` + `.opt`):**
- Requires debug build (`--source include/have_debug.inc`)
- 12 parallel worker connections (w1..w12) doing INSERT loops via
  `--send_eval CALL ins_table('tN', $rows_per_worker)` against 12
  narrow `INT PRIMARY KEY` tables
- `set(1)` #1 with writer at start of `ib_page_33` → truncated
  block_num=1883 persisted into ib_page_33's reset block
- Perl wait monitors archive file size every 10s, with a 60s
  no-growth watchdog
- When `ib_page_33` seals at 33,488,896 bytes AND `ib_page_34`
  appears with `< 160 blocks` (so the next slot is still in [1884, 2043]
  range and stays past ib_page_34's EOF), `set(1)` #2 fires
- `restart_mysqld.inc` — face A assertion (today: recovery crash)
- `mysqlbackup_page_track_get_changed_pages` + `SELECT 1` — face B
  assertion (today: crash + disconnect)
- master.opt: BP=4G, redo=8G, `flush_log_at_trx_commit=0`,
  **`doublewrite=OFF`** (doubles write bandwidth), max_dirty_pct=10
  lwm=0, io_capacity=20000 / io_capacity_max=80000, lru_scan_depth=8192,
  adaptive_flushing_lwm=70, flush_neighbors=0, page_cleaners=8,
  `log-error-verbosity=3` (so the diagnostic prints actually appear)

**Alternative shell+sysbench repro (`run_natural_repro.sh`):**
- Standalone bash, no MTR. Direct mysqld lifecycle.
- Release build (no debug knobs in the workload).
- 64-thread sysbench `oltp_write_only`, 100 tables × 1M rows.
- WORKDIR defaults to `/home/satya/ps11175_natural` — edit if needed.

## How to run on this machine

```sh
# clone + checkout
cd ~  # or wherever you keep code
git clone https://github.com/satya-bodapati/percona-server.git
cd percona-server
git checkout ps_11175_natural_repro

# build debug for MTR (boost lib expected under ./boost; adjust path)
mkdir bld && cd bld
cmake -DWITH_DEBUG=1 -DWITH_BOOST=../boost ..
make -j$(nproc) mysqld

# run the MTR test (~5–15 minutes on a fast disk)
cd mysql-test
./mtr --big-test --force --max-test-fail=1 percona.ps_11175_natural_repro
```

If MTR is unsuitable for some reason, the shell-driven path is:

```sh
# in a release build dir (bld_rel), build mysqld
# then from repo root:
./run_natural_repro.sh
```

## What "the previous machine" got stuck on

Disk write bandwidth was ~42 MB/sec with doublewrite ON (or ~80 MB/sec
expected after turning it OFF). Cleaner output capped at ~2,600
pages/sec sustained. With 5.5M unique page flushes needed to fill
`ib_page_33`, that was ~35 min on that hardware.

The MTR test's `.opt` file now has `--innodb_doublewrite=OFF`, so this
machine should at minimum match that. If your disk is genuinely faster,
the cleaner should saturate higher and the test will finish in
single-digit minutes.

## Failure mode signatures to expect TODAY

- **Face A (restart fails):** `[MY-012642] Tried to read 32 bytes at
  offset N` in the recovery log when the test runs
  `--source include/restart_mysqld.inc`. MTR will time out on the
  restart and report the test as failed.
- **Face B (SELECT crashes):** Won't be reached today because face A
  fires first. After the `mach_write_to_2` fix, both should pass.

## Pitfalls already encountered (avoid repeating)

- `--send` in MTR doesn't substitute `$vars`. Use `--send_eval`. ✓ already fixed
- `INSTALL COMPONENT` requires user grants. Don't use `--skip-grant-tables`. ✓ already fixed
- `innodb_io_capacity` must be `<= innodb_io_capacity_max`. When using
  `SET GLOBAL`, set `_max` first. (At startup via .opt, this is fine.) ✓ already fixed
- `mysqlbackup_page_track_get_changed_pages` requires a non-empty
  digit-only `mysqlbackup_backup_id`. ✓ already in test
- Page-tracking diagnostics use `ib::info()` (Note level) — invisible
  unless `log-error-verbosity=3`. ✓ already in .opt
- Workdir for `run_natural_repro.sh` should NOT be `/tmp` if `/tmp`
  is small — sysbench can fill ~20 GB. Default is `/home/satya/...`;
  edit if needed.

## Code locations for quick navigation

| What | File:line |
|---|---|
| The bug | `storage/innobase/arch/arch0page.cc:1458` (the `mach_write_to_2` call) |
| Recovery side of bug | `storage/innobase/arch/arch0recv.cc:749` (`fetch_reset_lsn`) |
| Seed knob | `storage/innobase/arch/arch0page.cc:2467` |
| File-index alignment for seeded case | same file, ~`:2504` |
| Diagnostic prints | grep for `page_archiver:` in `arch0page.cc` / `arch0arch.cc` |
| Test scenario | `mysql-test/suite/percona/t/ps_11175_natural_repro.test` |
| Shell repro | `run_natural_repro.sh` (repo root) |

## On memory

The previous Claude session has user/feedback/project memories saved
locally. None of them transfer over MCP / network. If the user wants
this session to remember any preferences they stated, they'll need to
restate them — or you can offer to write a memory file in this session
based on what they share.

Particularly load-bearing prior feedback (paraphrased — verify with
the user if it matters):
- always invoke `./mtr` from the `bld*/mysql-test` build directory,
  not the source tree
- don't use `--record` on MTR tests; prefer no `.result` and
  self-validate via `--error` and `search_pattern.inc`
- tests should fail today (bug present) and pass after fix
- sealed older archive files are immutable; don't theorise partial
  data_len at crash time for them
