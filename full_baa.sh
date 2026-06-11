#!/bin/bash
# PS-11175 FULL customer sequence on clean 8.0 + bombard + PS-11221 hang fix +
# PS-11247 m_count fix. One datadir lineage, all natural (no rm):
#   bombard past 65535, plant L_MID (low purge anchor) + E_B (truncated, derefs
#   ib_page_0, recovery-safe) -> restart #1 OK -> re-arm + plant E_A (past-EOF in
#   a fresh slot<160 tail) -> purge_up_to(L_MID) (real purge removes ib_page_0,
#   E_B survives) -> get_changed_pages(E_B) = FACE B (server exit) -> restart #2 =
#   FACE A (recovery reads E_A past EOF, MY-012642) -> restart #3 = FACE A again
#   (m_count fix => no spurious file/purge => stays fatal = unrestartable).
set -u
# Override BLD / WORKDIR via env, e.g.:  BLD=/path/to/bld WORKDIR=/tmp/ps11175 ./full_baa.sh
BLD=${BLD:-/mnt/storage/satya/percona-server/bld_rel}   # release build dir
MYSQLD=$BLD/runtime_output_directory/mysqld
MYSQL=$BLD/runtime_output_directory/mysql
WORKDIR=${WORKDIR:-/mnt/storage/satya/ps11175_baa}      # scratch datadir (needs ~6 GB)
DATADIR=$WORKDIR/data
SOCK=$WORKDIR/sock
ARCH_GLOB="$DATADIR/#ib_archive/page_group_*"
BOMBARD=16; SB_TABLES=20; SB_TABLE_SIZE=200000
log(){ echo "[$(date +%T)] $*"; }
sql(){ $MYSQL --socket=$SOCK -uroot --batch --skip-column-names "$@"; }
cur_block(){ local s=0 f; for f in $ARCH_GLOB/ib_page_*; do [ -e "$f" ] && s=$((s+$(stat -c%s "$f"))); done; echo $((s/16384)); }
tail_idx(){ ls $ARCH_GLOB/ib_page_* 2>/dev/null | sed 's/.*ib_page_//' | sort -n | tail -1; }
fsize(){ local f; f=$(echo $ARCH_GLOB/ib_page_$1); [ -e "$f" ] && stat -c%s "$f" || echo 0; }
files(){ ls $ARCH_GLOB/ib_page_* 2>/dev/null | sed 's#.*/##' | sort -V | tr '\n' ' '; }
last_reset(){ grep -a save_reset_point $WORKDIR/$1 2>/dev/null | tail -1 | sed 's/.*page_archiver://'; }
MPID=0
start(){ rm -f $SOCK; $MYSQLD --no-defaults --basedir=$BLD --datadir=$DATADIR --socket=$SOCK \
  --pid-file=$WORKDIR/pid --log-error=$WORKDIR/$1 --port=33382 --skip-networking --tmpdir=$WORKDIR/tmp \
  --disable-log-bin --innodb_buffer_pool_size=4G --innodb_buffer_pool_instances=8 --innodb_redo_log_capacity=8G \
  --innodb_flush_log_at_trx_commit=0 --innodb_page_cleaners=8 --innodb_doublewrite=OFF \
  --innodb_arch_page_bombard=$BOMBARD --max_connections=512 --log-error-verbosity=3 --skip-mysqlx \
  --skip-name-resolve --user=$(whoami) >> $WORKDIR/stdout.log 2>&1 &
  MPID=$!; for i in $(seq 1 180); do [ -S $SOCK ] && return 0; kill -0 $MPID 2>/dev/null || return 1; sleep 1; done; return 1; }
stop(){ sql -e "SHUTDOWN;" 2>/dev/null; for i in $(seq 1 240); do kill -0 $MPID 2>/dev/null || return 0; sleep 1; done; return 1; }
run_sb(){ sysbench oltp_write_only --db-driver=mysql --mysql-socket=$SOCK --mysql-user=root --mysql-db=sbtest \
  --tables=$SB_TABLES --table-size=$SB_TABLE_SIZE --threads=$1 --time=$2 --report-interval=10 run >> $WORKDIR/sb.log 2>&1 & SB_PID=$!; }
kill_sb(){ kill -9 $SB_PID 2>/dev/null; wait $SB_PID 2>/dev/null; }

log "=== Phase 1: init + boot + prepare ==="
rm -rf $WORKDIR; mkdir -p $DATADIR $WORKDIR/tmp
$MYSQLD --no-defaults --basedir=$BLD --datadir=$DATADIR --initialize-insecure --innodb_redo_log_capacity=64M --user=$(whoami) > $WORKDIR/init.log 2>&1
start boot.err || { log FATAL; tail -15 $WORKDIR/boot.err; exit 1; }
sql -e "INSTALL COMPONENT 'file://component_mysqlbackup';"; sql -e "CREATE DATABASE sbtest;"
sql -e "SET GLOBAL innodb_io_capacity_max=200000; SET GLOBAL innodb_io_capacity=40000; SET GLOBAL innodb_max_dirty_pages_pct=1; SET GLOBAL innodb_max_dirty_pages_pct_lwm=0;"
sysbench oltp_write_only --db-driver=mysql --mysql-socket=$SOCK --mysql-user=root --mysql-db=sbtest --tables=$SB_TABLES --table-size=$SB_TABLE_SIZE --threads=32 prepare > $WORKDIR/prep.log 2>&1
TRACK=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "TRACK_LSN=$TRACK"

log "=== Phase 2: bombard, plant L_MID + E_B ==="
run_sb 32 3600; LMID=""; EB=""; last=0; stuck=0
while true; do
  blk=$(cur_block)
  log "  block~$blk tail=ib_page_$(tail_idx)"
  if [ -z "$LMID" ] && [ "$blk" -ge 9000 ]; then LMID=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "  >>> L_MID lsn=$LMID $(last_reset boot.err)"; fi
  if [ -n "$LMID" ] && [ -z "$EB" ] && [ "$blk" -ge 65700 ]; then EB=$(sql -e "SELECT mysqlbackup_page_track_set(1);"); log "  >>> E_B lsn=$EB $(last_reset boot.err)"; fi
  if [ -n "$EB" ] && [ "$blk" -ge 69000 ]; then log "  E_B host sealed (~$blk)"; break; fi
  if [ "$blk" -eq "$last" ]; then stuck=$((stuck+1)); else stuck=0; last=$blk; fi
  [ $stuck -ge 24 ] && { log "  WARN stalled ~$blk"; break; }
  kill -0 $SB_PID 2>/dev/null || { log "  sysbench exited"; break; }
  sleep 5
done
kill_sb; sleep 2
{ [ -n "$LMID" ] && [ -n "$EB" ]; } || { log "FAIL missing LMID/EB"; stop; exit 2; }

log "=== Phase 3: shutdown + restart #1 (recovery-safe) ==="
stop || { log "FATAL shutdown"; exit 1; }
start restart1.err || { log "restart1 FAILED"; grep -aE "MY-012642|MY-013581" $WORKDIR/restart1.err|tail -4; exit 3; }
log "  restart #1 OK"

log "=== Phase 4: plant E_A (past-EOF, slot<160 fresh tail) ==="
# Post-restart the track_page dedup makes the workload alone advance m_block_num
# only by the small dirty backlog (it re-dirties already-tracked pages), so the
# tail may not roll to a fresh file within a fixed window. Instead, FIRE set(1)
# every iteration: each call re-arms track_page_lsn and claims the current dirty
# set, so the cleaner steadily advances m_block_num. The set(1) that lands in a
# brand-new tail file at slot<160 (file size < ~2.6 MB) IS E_A: its truncated
# block_num wraps ~30 MB past that small file's EOF -> recovery read past EOF.
sql -e "SET GLOBAL innodb_arch_page_bombard=150;"           # widen slot<160 window
T0=$(tail_idx); run_sb 32 1800; EA=""
for i in $(seq 1 1800); do
  CAND=$(sql -e "SELECT mysqlbackup_page_track_set(1);")    # advance via backlog + candidate reset
  t=$(tail_idx)
  if [ "$t" -gt "$T0" ] 2>/dev/null && [ "$(fsize $t)" -lt 2621440 ]; then
    EA=$CAND; log "  >>> E_A in ib_page_$t size=$(fsize $t) lsn=$EA"; log "      $(last_reset restart1.err)"; break
  fi
  [ $((i % 30)) -eq 0 ] && log "  ...advancing; tail=ib_page_$t size=$(fsize $t)"
  kill -0 $SB_PID 2>/dev/null || break
  sleep 1
done
kill_sb; sleep 2
[ -n "$EA" ] || { log "FAIL no E_A (tail=ib_page_$(tail_idx) size=$(fsize $(tail_idx)))"; stop; exit 4; }

log "=== Phase 5: FACE B -- purge_up_to(L_MID) + get_changed_pages(E_B) ==="
sql -e "SET GLOBAL mysqlbackup.backupid='12345';"
log "  files before purge: $(files)"
log "  purge_up_to($LMID) -> $(sql -e "SELECT mysqlbackup_page_track_purge_up_to($LMID);" 2>&1)"
log "  files after  purge: $(files)"
sql -e "SELECT mysqlbackup_page_track_get_changed_pages($EB,0);" > $WORKDIR/faceb.out 2>&1
log "  client: $(cat $WORKDIR/faceb.out)"; sleep 3
if kill -0 $MPID 2>/dev/null; then FACEB=UP; log "  FACE B: server UP (no crash)"; stop
else FACEB=CRASH; log "  FACE B: server DEAD (exit):"; grep -aE "MY-012646|OS error|Cannot continue|ib_page_0" $WORKDIR/restart1.err | tail -4 | sed 's/^/    /'; fi

log "=== Phase 6: FACE A -- restart attempts (expect MY-012642 every time) ==="
for n in 1 2 3; do
  if start faceA$n.err; then log "  restart #$n: came UP"; sql -e "SELECT 1;">/dev/null 2>&1 && log "    SELECT 1 ok"; stop
  else log "  restart #$n: did NOT come up"; grep -aE "MY-012642|MY-012646|MY-013581|Tried to read|Cannot continue|rmdir" $WORKDIR/faceA$n.err | tail -3 | sed 's/^/    /'; fi
done

log "=== SUMMARY ==="
log "  TRACK=$TRACK L_MID=$LMID E_B=$EB E_A=$EA  Face B=$FACEB"
for n in 1 2 3; do log "  faceA restart$n: MY-012642=$(grep -ac MY-012642 $WORKDIR/faceA$n.err 2>/dev/null) up=$(grep -ac 'ready for connections' $WORKDIR/faceA$n.err 2>/dev/null) purge=$(grep -ac 'rmdir' $WORKDIR/faceA$n.err 2>/dev/null)"; done
log "DONE"
