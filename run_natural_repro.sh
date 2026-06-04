#!/bin/bash
# PS-11175 natural-workload repro driven by shell + sysbench.
# Release build (no leaf-limit knob, no MTR), 64-thread sysbench OLTP update
# workload, manual mysqld lifecycle. Goal: fill ib_page_33 to 33 MB, drop a
# second set(1) into ib_page_34 while it's still partial, restart mysqld, run
# get_changed_pages. Each stage's outcome is captured in the workdir.

set -euo pipefail

# ------- Config -------
BLD=/home/satya/WORK/PS-8.0-natural-repro/bld_rel
MYSQLD=$BLD/runtime_output_directory/mysqld
MYSQL=$BLD/runtime_output_directory/mysql
WORKDIR=/home/satya/ps11175_natural
DATADIR=$WORKDIR/data
SOCK=$WORKDIR/mysqld.sock
PIDFILE=$WORKDIR/mysqld.pid
LOG=$WORKDIR/mysqld.err
PROGRESS=$WORKDIR/progress.log
ARCHIVE_DIR_GLOB="$DATADIR/#ib_archive/page_group_*"

# Sysbench parameters
SB_TABLES=100
SB_TABLE_SIZE=1000000        # 100 tables x 1M rows = ~20GB > 16GB BP -> forces eviction-driven flushing
SB_THREADS=64
SB_TIME=3600                 # generous; we'll stop it externally when ib_page_33 fills

# ------- Helpers -------
log() {
  local line="[$(date +%T)] $*"
  echo "$line"
  if [ -d "$WORKDIR" ]; then echo "$line" >> $PROGRESS; fi
}
mysql_run() {
  $MYSQL --socket=$SOCK --user=root --batch --skip-column-names "$@"
}

start_server() {
  log "Starting mysqld..."
  # Big BP + big redo + commit-fsync off. Flush-side knobs (dirty_pct,
  # io_capacity, lru_scan_depth, adaptive_flushing_lwm) are left at their
  # defaults at startup so we can SEE the impact of live tuning. Only the
  # startup-only knobs (BP size, BP instances, redo, page_cleaners, seed)
  # are set here.
  $MYSQLD --no-defaults \
    --basedir=$BLD \
    --datadir=$DATADIR \
    --socket=$SOCK \
    --pid-file=$PIDFILE \
    --log-error=$LOG \
    --port=33371 \
    --skip-networking \
    --tmpdir=$WORKDIR/tmp \
    --secure-file-priv=$WORKDIR \
    --innodb_buffer_pool_size=16G \
    --innodb_buffer_pool_instances=16 \
    --innodb_redo_log_capacity=10G \
    --innodb_flush_log_at_trx_commit=0 \
    --innodb_page_cleaners=16 \
    --innodb_arch_page_initial_block_num=67419 \
    --innodb_doublewrite=OFF \
    --max_prepared_stmt_count=1000000 \
    --max_connections=512 \
    --log-error-verbosity=3 \
    --skip-mysqlx \
    --skip-name-resolve \
    --user=$(whoami) \
    > $WORKDIR/stdout.log 2> $WORKDIR/stderr.log &
  MYSQLD_PID=$!
  log "mysqld PID=$MYSQLD_PID"
  for i in $(seq 1 60); do
    if [ -S $SOCK ]; then log "Server up after ${i}s"; return 0; fi
    if ! kill -0 $MYSQLD_PID 2>/dev/null; then
      log "ERROR: mysqld died during startup, see $LOG"; tail -20 $LOG; exit 1
    fi
    sleep 1
  done
  log "ERROR: server didn't come up within 60s"; exit 1
}

stop_server() {
  log "Shutting down mysqld..."
  mysql_run -e "SHUTDOWN;" 2>/dev/null || true
  for i in $(seq 1 30); do
    if [ ! -S $SOCK ]; then log "Server stopped after ${i}s"; return 0; fi
    sleep 1
  done
  log "Server didn't stop in 30s; killing"
  kill -KILL $(cat $PIDFILE) 2>/dev/null || true
}

archive_size() {
  local f
  f=$(echo $ARCHIVE_DIR_GLOB/ib_page_33 2>/dev/null)
  if [ -e "$f" ]; then stat -c%s "$f"; else echo 0; fi
}

archive_size_n() {
  # arg1 = file index (33 / 34)
  local f
  f=$(echo $ARCHIVE_DIR_GLOB/ib_page_$1 2>/dev/null)
  if [ -e "$f" ]; then stat -c%s "$f"; else echo 0; fi
}

# ------- Phase 1: clean workdir + initialize datadir -------
log "===== Phase 1: workdir setup ====="
rm -rf $WORKDIR
mkdir -p $DATADIR $WORKDIR/tmp
log "Initializing fresh datadir..."
$MYSQLD --no-defaults --basedir=$BLD --datadir=$DATADIR --initialize-insecure \
  --innodb_redo_log_capacity=10M --user=$(whoami) > $WORKDIR/init.log 2>&1
log "Datadir initialized."

# ------- Phase 2: start server -------
log "===== Phase 2: start mysqld ====="
start_server

# ------- Phase 3: setup -------
log "===== Phase 3: install component + create sysbench db ====="
mysql_run -e "INSTALL COMPONENT 'file://component_mysqlbackup';"
mysql_run -e "CREATE DATABASE sbtest;"

# ------- Phase 3a: live-tune flushing aggressively (BEFORE prepare!) -------
# Prepare generates ~100 MB/sec of redo (100M-row insert). With default
# io_capacity_max=2000 the cleaner does ~32 MB/sec, and redo fills faster
# than checkpoint can drain -> MY-014084 redo-pressure warnings. Apply the
# aggressive flushing BEFORE prepare so the cleaner can keep up.
log "===== Phase 3a: live tune flushing aggressively via SET GLOBAL ====="
# Order matters: innodb_io_capacity must be <= innodb_io_capacity_max,
# so set _max first (otherwise setting capacity above default _max=2000
# silently fails and we end up with capacity=2000 too).
mysql_run -e "
  SET GLOBAL innodb_io_capacity_max = 100000;
  SET GLOBAL innodb_io_capacity = 40000;
  SET GLOBAL innodb_max_dirty_pages_pct = 10;
  SET GLOBAL innodb_max_dirty_pages_pct_lwm = 0;
  SET GLOBAL innodb_lru_scan_depth = 16384;
  SET GLOBAL innodb_adaptive_flushing_lwm = 70;
  SET GLOBAL innodb_flush_neighbors = 0;
"
log "Live flush tuning applied. To re-tune while workload runs, connect with:"
log "  $MYSQL --socket=$SOCK --user=root"

# ------- Phase 4: sysbench prepare BEFORE tracking starts -------
# Done before set(1) so the prepare's page flushes are NOT in the archive.
# Once tracking starts, only the UPDATE workload's flushes are tracked.
log "===== Phase 4: sysbench prepare (${SB_TABLES} tables x ${SB_TABLE_SIZE} rows) ====="
SB_BASE=(
  --db-driver=mysql
  --mysql-socket=$SOCK
  --mysql-user=root
  --mysql-db=sbtest
  --tables=$SB_TABLES
  --table-size=$SB_TABLE_SIZE
)
# Parallel prepare so 100M-row dataset doesn't take ~30 min sequentially
sysbench oltp_update_index "${SB_BASE[@]}" --threads=$SB_THREADS prepare \
  > $WORKDIR/sb_prepare.log 2>&1
log "Prepare done: $(grep -c '^Creating' $WORKDIR/sb_prepare.log) tables"

# ------- Phase 5: first set(1) -- starts tracking -------
log "===== Phase 5: set(1) #1 -- writer seeded at block 67419 ====="
START_LSN=$(mysql_run -e "SELECT mysqlbackup_page_track_set(1);")
log "  start_lsn = $START_LSN"

# ------- Phase 6: launch sysbench oltp_write_only workload, monitor archive growth -------
# write_only dirties more pages per query than update_index alone:
# each transaction does ~4 UPDATEs, 1 INSERT, 1 DELETE -- a mix that touches
# PK leaves + secondary-index leaves + new-page allocations + tombstones.
log "===== Phase 6: sysbench oltp_write_only --threads=${SB_THREADS} ====="
sysbench oltp_write_only "${SB_BASE[@]}" \
  --threads=$SB_THREADS --time=$SB_TIME --report-interval=5 \
  run > $WORKDIR/sb_run.log 2>&1 &
SB_PID=$!
log "  sysbench PID=$SB_PID"

# Monitor archive size every 5s until ib_page_33 is full (33,488,896 bytes)
log "Monitoring archive growth (waiting for ib_page_33 to seal at 33,488,896 bytes)..."
last_sz=-1
stuck_iters=0
ib33_full=0
while true; do
  sz33=$(archive_size_n 33)
  sz34=$(archive_size_n 34)
  blocks33=$((sz33 / 16384))
  pct=$(awk "BEGIN { printf \"%.1f\", 100.0 * $sz33 / 33488896 }")
  printf "[t=%4ds] ib_page_33=%d bytes (%d blocks, %s%%)  ib_page_34=%d bytes\n" \
    $SECONDS $sz33 $blocks33 $pct $sz34 | tee -a $PROGRESS
  if [ $sz33 -eq 33488896 ] && [ $sz34 -gt 0 ]; then
    log "ib_page_33 sealed at 33,488,896 bytes; ib_page_34 exists (size=$sz34). Done after ${SECONDS}s."
    ib33_full=1
    break
  fi
  if [ $sz33 -eq $last_sz ]; then
    stuck_iters=$((stuck_iters+1))
    if [ $stuck_iters -ge 12 ] && [ $sz33 -gt 0 ] && [ $sz33 -lt 33488896 ]; then
      log "ERROR: ib_page_33 stuck at $sz33 bytes for 60s. Aborting workload."
      break
    fi
  else
    stuck_iters=0
    last_sz=$sz33
  fi
  if ! kill -0 $SB_PID 2>/dev/null; then
    log "sysbench exited (PID $SB_PID is gone). Check $WORKDIR/sb_run.log"
    break
  fi
  sleep 5
done

# Stop sysbench cleanly
log "Stopping sysbench..."
kill -INT $SB_PID 2>/dev/null || true
wait $SB_PID 2>/dev/null || true

if [ $ib33_full -ne 1 ]; then
  log "FAIL: workload finished before ib_page_33 sealed. Last size: $(archive_size_n 33) bytes."
  exit 2
fi

# ------- Phase 7: confirm ib_page_34 small + drop second set(1) -------
log "===== Phase 7: second set(1) into ib_page_34 ====="
sleep 5   # cleaner drain
sz34=$(archive_size_n 34)
limit=$((160 * 16384))    # 2 MB threshold for "still in slot range [1884, 2043]"
log "  ib_page_34 = $sz34 bytes (limit for face A = $limit bytes)"
if [ $sz34 -ge $limit ]; then
  log "WARN: ib_page_34 has grown past 160 blocks (2 MB). The next set(1)'s slot may wrap into [1, 1883] and face A may not fire."
fi

mysql_run -e "SET GLOBAL mysqlbackup_backup_id = '12345';"
RESET2_LSN=$(mysql_run -e "SELECT mysqlbackup_page_track_set(1);")
log "  reset2_lsn = $RESET2_LSN"
sleep 3  # let reset block flush

# ------- Phase 8: shutdown + restart -- face A assertion -------
log "===== Phase 8: shutdown + restart (face A test) ====="
stop_server
log "Restarting mysqld..."
if start_server; then
  log "FACE A PASS: mysqld restarted cleanly."
else
  log "FACE A FAIL: mysqld could not restart -- recovery crash (expected today without the fix)."
  tail -60 $LOG | tee -a $PROGRESS
  exit 3
fi

# Quick liveness
mysql_run -e "SELECT 1;" | tee -a $PROGRESS

# ------- Phase 9: get_changed_pages -- face B assertion -------
log "===== Phase 9: get_changed_pages (face B test) ====="
mysql_run -e "SET GLOBAL mysqlbackup_backup_id = '12345';"
log "Calling mysqlbackup_page_track_get_changed_pages($START_LSN, 0)..."
if mysql_run -e "SELECT mysqlbackup_page_track_get_changed_pages($START_LSN, 0);" >> $PROGRESS 2>&1; then
  log "get_changed_pages returned (no crash)."
else
  log "get_changed_pages errored (server may have crashed)."
fi

# Post-call liveness
log "Post-call SELECT 1..."
if mysql_run -e "SELECT 1;" > /dev/null 2>&1; then
  log "FACE B PASS: server stayed up after get_changed_pages."
else
  log "FACE B FAIL: server is dead after get_changed_pages -- ENOENT crash (expected today)."
  tail -60 $LOG | tee -a $PROGRESS
  exit 4
fi

log "===== All phases passed ====="
stop_server
exit 0
