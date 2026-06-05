#!/bin/bash
# PS-11175 NATURAL FACE B (no manual file deletion). Plant a low intermediate
# reset (purge anchor) + a high TRUNCATED reset E_B that derefs a low file.
# Then a real purge_up_to(intermediate) removes the low files (incl E_B's deref
# target); E_B survives (above the boundary). SELECT get_changed_pages(E_B) then
# derefs the legitimately-purged low file -> does it crash (MY-012646) naturally?
set -u
BLD=/mnt/storage/satya/percona-server/bld_rel
MYSQLD=$BLD/runtime_output_directory/mysqld
MYSQL=$BLD/runtime_output_directory/mysql
WORKDIR=/mnt/storage/satya/ps11175_natfb
DATADIR=$WORKDIR/data
SOCK=$WORKDIR/sock
ARCH_GLOB="$DATADIR/#ib_archive/page_group_*"
BOMBARD=16; SB_TABLES=20; SB_TABLE_SIZE=200000
log(){ echo "[$(date +%T)] $*"; }
sql(){ $MYSQL --socket=$SOCK -uroot --batch --skip-column-names "$@"; }
cur_block(){ local s=0 f; for f in $ARCH_GLOB/ib_page_*; do [ -e "$f" ] && s=$((s+$(stat -c%s "$f"))); done; echo $((s/16384)); }
tail_idx(){ ls $ARCH_GLOB/ib_page_* 2>/dev/null | sed 's/.*ib_page_//' | sort -n | tail -1; }
files(){ ls $ARCH_GLOB/ib_page_* 2>/dev/null | sed 's#.*/##' | sort -V | tr '\n' ' '; }
last_reset(){ grep -a save_reset_point $WORKDIR/$1 2>/dev/null | tail -1 | sed 's/.*page_archiver://'; }
MPID=0
start(){ rm -f $SOCK; $MYSQLD --no-defaults --basedir=$BLD --datadir=$DATADIR --socket=$SOCK \
  --pid-file=$WORKDIR/pid --log-error=$WORKDIR/$1 --port=33381 --skip-networking --tmpdir=$WORKDIR/tmp \
  --disable-log-bin --innodb_buffer_pool_size=4G --innodb_buffer_pool_instances=8 --innodb_redo_log_capacity=8G \
  --innodb_flush_log_at_trx_commit=0 --innodb_page_cleaners=8 --innodb_doublewrite=OFF \
  --innodb_arch_page_bombard=$BOMBARD --max_connections=512 --log-error-verbosity=3 --skip-mysqlx \
  --skip-name-resolve --user=$(whoami) >> $WORKDIR/stdout.log 2>&1 &
  MPID=$!; for i in $(seq 1 180); do [ -S $SOCK ] && return 0; kill -0 $MPID 2>/dev/null || return 1; sleep 1; done; return 1; }
stop(){ sql -e "SHUTDOWN;" 2>/dev/null; for i in $(seq 1 240); do kill -0 $MPID 2>/dev/null || return 0; sleep 1; done; return 1; }
run_sb(){ sysbench oltp_write_only --db-driver=mysql --mysql-socket=$SOCK --mysql-user=root --mysql-db=sbtest \
  --tables=$SB_TABLES --table-size=$SB_TABLE_SIZE --threads=$1 --time=$2 --report-interval=10 run >> $WORKDIR/sb.log 2>&1 & SB_PID=$!; }

log "=== init + boot ==="
rm -rf $WORKDIR; mkdir -p $DATADIR $WORKDIR/tmp
$MYSQLD --no-defaults --basedir=$BLD --datadir=$DATADIR --initialize-insecure --innodb_redo_log_capacity=64M --user=$(whoami) > $WORKDIR/init.log 2>&1
start boot.err || { log FATAL; tail -15 $WORKDIR/boot.err; exit 1; }
sql -e "INSTALL COMPONENT 'file://component_mysqlbackup';"; sql -e "CREATE DATABASE sbtest;"
sql -e "SET GLOBAL innodb_io_capacity_max=200000; SET GLOBAL innodb_io_capacity=40000; SET GLOBAL innodb_max_dirty_pages_pct=1; SET GLOBAL innodb_max_dirty_pages_pct_lwm=0;"
sysbench oltp_write_only --db-driver=mysql --mysql-socket=$SOCK --mysql-user=root --mysql-db=sbtest --tables=$SB_TABLES --table-size=$SB_TABLE_SIZE --threads=32 prepare > $WORKDIR/prep.log 2>&1
TRACK=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "TRACK_LSN=$TRACK"

log "=== bombard: plant L_MID (low anchor, file ~5) then E_B (truncated, derefs low) ==="
run_sb 32 3600; LMID=""; EB=""; last=0; stuck=0
while true; do
  blk=$(cur_block); ti=$(tail_idx)
  log "  block~$blk tail=ib_page_$ti"
  if [ -z "$LMID" ] && [ "$blk" -ge 9000 ]; then LMID=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "  >>> L_MID(anchor) block~$blk lsn=$LMID  $(last_reset boot.err)"; fi
  if [ -n "$LMID" ] && [ -z "$EB" ] && [ "$blk" -ge 65700 ]; then EB=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "  >>> E_B(truncated) block~$blk lsn=$EB  $(last_reset boot.err)"; fi
  if [ -n "$EB" ] && [ "$blk" -ge 69000 ]; then log "  E_B host sealed (block~$blk)"; break; fi
  if [ "$blk" -eq "$last" ]; then stuck=$((stuck+1)); else stuck=0; last=$blk; fi
  [ $stuck -ge 24 ] && { log "  WARN stalled ~$blk"; break; }
  kill -0 $SB_PID 2>/dev/null || { log "  sysbench exited"; break; }
  sleep 5
done
kill -9 $SB_PID 2>/dev/null; wait $SB_PID 2>/dev/null; sleep 2
{ [ -n "$LMID" ] && [ -n "$EB" ]; } || { log "FAIL: missing LMID/EB"; stop; exit 2; }

log "=== shutdown + restart #1 (recovery-safe) ==="
stop || { log "FATAL shutdown"; exit 1; }
start restart1.err || { log "restart1 failed (E_B past-EOF?)"; grep -aE "MY-012642|MY-013581" $WORKDIR/restart1.err | tail -4; exit 3; }
log "  restart #1 OK; files: $(files)"

log "=== FACE B (NATURAL): purge_up_to(L_MID=$LMID), then get_changed_pages(E_B=$EB) ==="
sql -e "SET GLOBAL mysqlbackup.backupid='12345';"
log "  files before purge: $(files)"
PURGED=$(sql -e "SELECT mysqlbackup_page_track_purge_up_to($LMID);" 2>&1)
log "  purge_up_to($LMID) -> $PURGED"
log "  files after purge:  $(files)"
log "  E_B's reset (recovered/truncated) -> $(grep -a save_reset_point $WORKDIR/restart1.err | grep -i trunc | tail -2 | sed 's/.*page_archiver:/    /')"
sql -e "SELECT mysqlbackup_page_track_get_changed_pages($EB,0);" > $WORKDIR/faceb.out 2>&1
log "  client: $(cat $WORKDIR/faceb.out)"; sleep 3
if kill -0 $MPID 2>/dev/null; then log "  RESULT: server UP -> FACE B did NOT crash (graceful)"; stop
else log "  RESULT: server DEAD -> NATURAL FACE B fired:"; grep -aE "MY-012646|MY-012642|OS error|Cannot continue|ib_page_" $WORKDIR/restart1.err | tail -6 | sed 's/^/    /'; fi
log "DONE"
