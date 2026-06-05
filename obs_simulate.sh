#!/bin/bash
# PS-11175 simulate-branch OBSERVATION trial: start tracking, bombard across
# block 65535, take a few set(1) snapshots, and dump the truncation diagnostics
# + archive file layout. Purpose: learn the real truncated values / file sizes
# so the full face-B/face-A harness can be tuned. No restart, no purge here.
set -u
BLD=/mnt/storage/satya/percona-server/bld_rel
MYSQLD=$BLD/runtime_output_directory/mysqld
MYSQL=$BLD/runtime_output_directory/mysql
WORKDIR=/mnt/storage/satya/ps11175_obs
DATADIR=$WORKDIR/data
SOCK=$WORKDIR/mysqld.sock
ARCH_GLOB="$DATADIR/#ib_archive/page_group_*"
BOMBARD=16           # max entries per block (proven stable in trial 5)
SB_TABLES=20; SB_TABLE_SIZE=200000; SB_THREADS=32

log(){ echo "[$(date +%T)] $*"; }
sql(){ $MYSQL --socket=$SOCK -uroot --batch --skip-column-names "$@"; }
tail_idx(){ ls $ARCH_GLOB/ib_page_* 2>/dev/null | sed 's/.*ib_page_//' | sort -n | tail -1; }
nfiles(){ ls $ARCH_GLOB/ib_page_* 2>/dev/null | wc -l; }

log "init"; rm -rf $WORKDIR; mkdir -p $DATADIR $WORKDIR/tmp
$MYSQLD --no-defaults --basedir=$BLD --datadir=$DATADIR --initialize-insecure \
  --innodb_redo_log_capacity=64M --user=$(whoami) > $WORKDIR/init.log 2>&1
rm -f $SOCK
$MYSQLD --no-defaults --basedir=$BLD --datadir=$DATADIR --socket=$SOCK \
  --pid-file=$WORKDIR/pid --log-error=$WORKDIR/err.err --port=33376 --skip-networking \
  --tmpdir=$WORKDIR/tmp --disable-log-bin --innodb_buffer_pool_size=4G \
  --innodb_buffer_pool_instances=8 --innodb_redo_log_capacity=8G \
  --innodb_flush_log_at_trx_commit=0 --innodb_page_cleaners=8 \
  --innodb_doublewrite=OFF --innodb_arch_page_bombard=$BOMBARD --log-error-verbosity=3 --skip-mysqlx \
  --skip-name-resolve --user=$(whoami) >> $WORKDIR/stdout.log 2>&1 &
MPID=$!
for i in $(seq 1 120); do [ -S $SOCK ] && break; kill -0 $MPID 2>/dev/null || { log "start died"; tail -20 $WORKDIR/err.err; exit 1; }; sleep 1; done
log "up (pid $MPID)"
sql -e "INSTALL COMPONENT 'file://component_mysqlbackup';"
sql -e "CREATE DATABASE sbtest;"
sql -e "SET GLOBAL innodb_io_capacity_max=200000; SET GLOBAL innodb_io_capacity=40000;
        SET GLOBAL innodb_max_dirty_pages_pct=1; SET GLOBAL innodb_max_dirty_pages_pct_lwm=0;"
log "prepare"; sysbench oltp_write_only --db-driver=mysql --mysql-socket=$SOCK --mysql-user=root \
  --mysql-db=sbtest --tables=$SB_TABLES --table-size=$SB_TABLE_SIZE --threads=$SB_THREADS prepare > $WORKDIR/prep.log 2>&1

log "set(1) #start tracking"; S0=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "  start_lsn=$S0"
log "bombard=$BOMBARD on; workload"
sql -e "SET GLOBAL innodb_arch_page_bombard=$BOMBARD;"
sysbench oltp_write_only --db-driver=mysql --mysql-socket=$SOCK --mysql-user=root \
  --mysql-db=sbtest --tables=$SB_TABLES --table-size=$SB_TABLE_SIZE --threads=$SB_THREADS \
  --time=3600 --report-interval=10 run > $WORKDIR/run.log 2>&1 &
SB=$!
tot_bytes(){ local s=0 f; for f in $ARCH_GLOB/ib_page_*; do [ -e "$f" ] && s=$((s + $(stat -c%s "$f"))); done; echo $s; }
# Block 65535 => ~1.07 GB of archive. Watch total bytes climb; snapshot set(1)
# at a few block thresholds to capture the truncation once block > 65535.
SNAP=0; last=0; stuck=0
while true; do
  tb=$(tot_bytes); blk=$((tb / 16384)); ti=$(tail_idx); ti=${ti:-0}
  tps=$(grep -oE "tps: [0-9.]+" run.log 2>/dev/null | tail -1 | awk '{print $2}')
  log "  block~$blk (${tb}B, tail=ib_page_$ti) tps=$tps"
  if [ "$blk" -ge 64000 ] && [ $SNAP -eq 0 ]; then SNAP=1; X=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "  >>> set@block~$blk lsn=$X"; fi
  if [ "$blk" -ge 67000 ] && [ $SNAP -eq 1 ]; then SNAP=2; X=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "  >>> set@block~$blk lsn=$X"; fi
  if [ "$blk" -ge 69000 ]; then X=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "  >>> set@block~$blk lsn=$X (crossed 65535)"; break; fi
  if [ "$tb" -eq "$last" ]; then stuck=$((stuck+1)); else stuck=0; last=$tb; fi
  [ $stuck -ge 24 ] && { log "  stalled at block~$blk (${tb}B)"; break; }
  kill -0 $SB 2>/dev/null || { log "  sysbench exited"; break; }
  sleep 5
done
sql -e "SET GLOBAL innodb_arch_page_bombard=0;"
kill -9 $SB 2>/dev/null; wait $SB 2>/dev/null; sleep 3

log "===== ARCHIVE LAYOUT ====="
ls -la $ARCH_GLOB/ | grep -E "active|durable|ib_page" | awk '{print "  "$5"\t"$9}'
log "===== ALL save_reset_point (real vs truncated) ====="
grep -a "save_reset_point" $WORKDIR/err.err | sed 's/.*page_archiver:/  /'
log "===== file-creation events ====="
grep -ac "created data file" $WORKDIR/err.err | sed 's/^/  count=/'
log "===== writer block_num progression (sampled) ====="
grep -a "bombard: writer block_num" $WORKDIR/err.err | tail -5 | sed 's/.*bombard:/  /'
sql -e "SHUTDOWN;" 2>/dev/null
log "DONE (datadir kept at $WORKDIR for inspection)"
