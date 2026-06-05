#!/bin/bash
# PS-11175 reproduction on CLEAN 8.0 via the page-id BOMBARD knob
# (ps_11175_simulate_page_id: origin/8.0 + only innodb_arch_page_bombard, no
# seeding, no fixes). Proves the truncation bug with a FAITHFUL ib_page_0..N
# layout, then reproduces FACE A (restart fatally unrecoverable).
#
# Geometry: a reset's persisted block_num is mach_write_to_2-truncated once the
# real block_num > 65535. The truncated in-file slot = real_slot - 160 (since
# 65536 mod 2043 = 160). So when set(1) fires while the writer is in the FIRST
# ~160 blocks of a fresh archive file, the truncated slot WRAPS to ~1883 -> a
# read offset ~30 MB, far past that small partial file's EOF. On restart,
# recovery's fetch_reset_lsn reads 32 bytes past EOF -> MY-012642 -> the server
# will not start, and every retry fails the same way (unrestartable).
set -u

BLD=/mnt/storage/satya/percona-server/bld_rel
MYSQLD=$BLD/runtime_output_directory/mysqld
MYSQL=$BLD/runtime_output_directory/mysql
WORKDIR=/mnt/storage/satya/ps11175_simulate
DATADIR=$WORKDIR/data
SOCK=$WORKDIR/mysqld.sock
ARCH_GLOB="$DATADIR/#ib_archive/page_group_*"
BOMBARD=16
SB_TABLES=20; SB_TABLE_SIZE=200000

log(){ echo "[$(date +%T)] $*"; }
sql(){ $MYSQL --socket=$SOCK -uroot --batch --skip-column-names "$@"; }
tot_bytes(){ local s=0 f; for f in $ARCH_GLOB/ib_page_*; do [ -e "$f" ] && s=$((s+$(stat -c%s "$f"))); done; echo $s; }
cur_block(){ echo $(( $(tot_bytes) / 16384 )); }
tail_idx(){ ls $ARCH_GLOB/ib_page_* 2>/dev/null | sed 's/.*ib_page_//' | sort -n | tail -1; }
fsize(){ local f; f=$(echo $ARCH_GLOB/ib_page_$1); [ -e "$f" ] && stat -c%s "$f" || echo 0; }

MYSQLD_PID=0
start_server(){  # $1 = error log basename
  rm -f $SOCK
  $MYSQLD --no-defaults --basedir=$BLD --datadir=$DATADIR --socket=$SOCK \
    --pid-file=$WORKDIR/mysqld.pid --log-error=$WORKDIR/$1 --port=33377 \
    --skip-networking --tmpdir=$WORKDIR/tmp --disable-log-bin \
    --innodb_buffer_pool_size=4G --innodb_buffer_pool_instances=8 \
    --innodb_redo_log_capacity=8G --innodb_flush_log_at_trx_commit=0 \
    --innodb_page_cleaners=8 --innodb_doublewrite=OFF \
    --innodb_arch_page_bombard=$BOMBARD --max_connections=512 \
    --log-error-verbosity=3 --skip-mysqlx --skip-name-resolve \
    --user=$(whoami) >> $WORKDIR/stdout.log 2>&1 &
  MYSQLD_PID=$!
  for i in $(seq 1 180); do
    [ -S $SOCK ] && return 0
    kill -0 $MYSQLD_PID 2>/dev/null || return 1
    sleep 1
  done
  return 1
}
stop_server(){
  sql -e "SHUTDOWN;" 2>/dev/null
  for i in $(seq 1 300); do kill -0 $MYSQLD_PID 2>/dev/null || return 0; sleep 1; done
  return 1
}
run_sb(){  # $1=threads $2=seconds ; sets SB_PID
  sysbench oltp_write_only --db-driver=mysql --mysql-socket=$SOCK --mysql-user=root \
    --mysql-db=sbtest --tables=$SB_TABLES --table-size=$SB_TABLE_SIZE \
    --threads=$1 --time=$2 --report-interval=10 run >> $WORKDIR/sb_run.log 2>&1 &
  SB_PID=$!
}
kill_sb(){ kill -9 $SB_PID 2>/dev/null; wait $SB_PID 2>/dev/null; }

# ---------- Phase 1: init + boot ----------
log "===== Phase 1: init datadir ====="
rm -rf $WORKDIR; mkdir -p $DATADIR $WORKDIR/tmp
$MYSQLD --no-defaults --basedir=$BLD --datadir=$DATADIR --initialize-insecure \
  --innodb_redo_log_capacity=64M --user=$(whoami) > $WORKDIR/init.log 2>&1
start_server boot.err || { log "FATAL: boot failed"; tail -20 $WORKDIR/boot.err; exit 1; }
sql -e "INSTALL COMPONENT 'file://component_mysqlbackup';"
sql -e "CREATE DATABASE sbtest;"
sql -e "SET GLOBAL innodb_io_capacity_max=200000; SET GLOBAL innodb_io_capacity=40000;
        SET GLOBAL innodb_max_dirty_pages_pct=1; SET GLOBAL innodb_max_dirty_pages_pct_lwm=0;
        SET GLOBAL innodb_flush_neighbors=0;"
log "server up (pid $MYSQLD_PID)"

# ---------- Phase 2: prepare ----------
log "===== Phase 2: sysbench prepare ====="
sysbench oltp_write_only --db-driver=mysql --mysql-socket=$SOCK --mysql-user=root \
  --mysql-db=sbtest --tables=$SB_TABLES --table-size=$SB_TABLE_SIZE --threads=32 \
  prepare > $WORKDIR/sb_prep.log 2>&1
log "prepare done"

# ---------- Phase 3: start tracking ----------
TRACK_LSN=$(sql -e "SELECT mysqlbackup_page_track_set(1);")
log "===== Phase 3: tracking started, TRACK_LSN=$TRACK_LSN ====="

# ---------- Phase 4: bombard across block 65535 (fast, 32 threads) ----------
log "===== Phase 4: bombard across 65535 (32 threads) ====="
run_sb 32 3600
last=0; stuck=0
while true; do
  blk=$(cur_block)
  log "  block~$blk tail=ib_page_$(tail_idx)"
  [ "$blk" -ge 66200 ] && { log "  crossed 65535 (block~$blk)"; break; }
  if [ "$blk" -eq "$last" ]; then stuck=$((stuck+1)); else stuck=0; last=$blk; fi
  [ $stuck -ge 24 ] && { log "  WARN: stalled at block~$blk"; break; }
  kill -0 $SB_PID 2>/dev/null || { log "  sysbench exited"; break; }
  sleep 5
done
kill_sb
[ "$(cur_block)" -ge 66000 ] || { log "FAIL: did not cross 65535"; stop_server; exit 2; }

# ---------- Phase 5: catch a fresh-file roll, plant past-EOF reset E_A ----------
# Slow workload (2 threads) so blocks roll slowly and we can fire set(1) while
# the writer is within the first ~160 blocks of a brand-new file.
log "===== Phase 5: catch slot<160 of a fresh file -> E_A (past-EOF) ====="
T0=$(tail_idx)
run_sb 2 600
EA_LSN=""
for i in $(seq 1 400); do
  t=$(tail_idx)
  if [ "$t" -gt "$T0" ] 2>/dev/null; then
    sz=$(fsize $t)
    if [ "$sz" -lt 2621440 ]; then   # < 160 blocks => slot<160 window
      EA_LSN=$(sql -e "SELECT mysqlbackup_page_track_set(1);")
      log "  >>> E_A set(1) in ib_page_$t at size=$sz : EA_LSN=$EA_LSN"
      grep -a "save_reset_point" $WORKDIR/boot.err | tail -1 | sed 's/.*page_archiver:/      /'
      break
    fi
  fi
  kill -0 $SB_PID 2>/dev/null || { log "  sysbench exited during catch"; break; }
  sleep 0.3
done
kill_sb; sleep 3
log "  tail now ib_page_$(tail_idx) size=$(fsize $(tail_idx))"
[ -n "$EA_LSN" ] || { log "FAIL: could not plant E_A in slot<160 window"; stop_server; exit 4; }

# ---------- Phase 6: clean shutdown ----------
log "===== Phase 6: clean shutdown ====="
stop_server || { log "FATAL: shutdown hung"; exit 1; }

# ---------- Phase 7: FACE A -- restart attempts (expect MY-012642, unrestartable) ----------
log "===== Phase 7: FACE A -- restart attempts ====="
for n in 1 2 3; do
  if start_server faceA_restart$n.err; then
    log "  restart #$n: came UP (face A did NOT fire on this attempt)"
    sql -e "SELECT 1;" >/dev/null 2>&1 && log "    SELECT 1 ok"
    stop_server
  else
    log "  restart #$n: did NOT come up"
    grep -aE "MY-012642|MY-012646|MY-013581|Tried to read|Cannot continue" $WORKDIR/faceA_restart$n.err | tail -4 | sed 's/^/      /'
  fi
done

log "===== SUMMARY ====="
log "  TRACK_LSN=$TRACK_LSN EA_LSN=$EA_LSN"
for n in 1 2 3; do
  log "  restart$n: MY-012642=$(grep -ac MY-012642 $WORKDIR/faceA_restart$n.err 2>/dev/null) came_up=$(grep -ac 'ready for connections' $WORKDIR/faceA_restart$n.err 2>/dev/null)"
done
log "DONE"
