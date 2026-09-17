#!/bin/bash
# Per-commit gate for the v4 split. Every commit from BASE to the tip of the
# transfer branch must: build, be clang-format clean over the lines it changed,
# and run the vector suite with zero failures.
# No destructive commands: this never removes build dirs or untracked files.
set -u
REPO=~/martin-vector
BLD=$REPO/bld-debug
BASE=ecb908769a5
BRANCH=${1:-vec-transfer-v4}
LOG=/tmp/gate_v4.log
# One gate at a time: two of them share a build directory, which produces
# truncated object files and a log with interleaved results.
exec 9>/tmp/gate_v4.lock
if ! flock -n 9; then
  echo "another gate is already running" >&2
  exit 1
fi
: > $LOG

cd $REPO || exit 1
git fetch -q fork "$BRANCH" 2>&1 | tail -1 >> $LOG
TIP=$(git rev-parse FETCH_HEAD)
echo "gating $BRANCH $TIP" >> $LOG

for c in $(git rev-list --reverse $BASE..$TIP); do
  short=$(git rev-parse --short=11 $c)
  subj=$(git log -1 --format=%s $c | sed 's/^PS-11299: //')
  # a commit that already gated GREEN cannot change: skip the rebuild
  if [ -f /tmp/green_$short ]; then
    echo "$short GREEN (cached) | $subj" >> $LOG
    continue
  fi
  git checkout -q --detach $c 2>/dev/null || { echo "$short checkout=FAIL | $subj" >> $LOG; continue; }

  # format: same check the pre-commit hook runs, over this commit's own diff.
  # empty output = the changed lines are already formatted.
  FMT=$(~/apply-format $c~1 2>/dev/null | wc -l)

  if ! make -C $BLD -j128 > /tmp/build_$short.log 2>&1; then
    echo "$short build=FAIL fmt=$FMT | $subj" >> $LOG
    grep -m5 -E "error:" /tmp/build_$short.log >> $LOG
    grep -m3 "undefined reference" /tmp/build_$short.log >> $LOG
    echo "STOPPED at first failure" >> $LOG
    break
  fi

  cd $BLD/mysql-test
  rm -rf var
  perl ./mysql-test-run.pl --suite=percona --do-test=vector \
       --parallel=64 --force --max-test-fail=0 --nowarnings > /tmp/mtr_$short.log 2>&1
  P=$(grep -cE '\[ pass \]' /tmp/mtr_$short.log)
  FAILED=$(grep -oE 'percona\.[a-z0-9_]+ +w[0-9]+ +\[ fail' /tmp/mtr_$short.log \
           | awk '{print $1}' | sed 's/percona\.//' | sort -u | tr '\n' ' ')
  F=$(echo $FAILED | wc -w)
  mkdir -p /tmp/rej_$short
  find var -name '*.reject' -exec cp {} /tmp/rej_$short/ \; 2>/dev/null
  cd $REPO
  VERDICT=GREEN
  [ "$F" != 0 ] && VERDICT=RED
  [ "$FMT" != 0 ] && VERDICT=RED
  [ "$VERDICT" = GREEN ] && touch /tmp/green_$short
  echo "$short $VERDICT build=ok fmt=$FMT pass=$P fail=$F [$FAILED] | $subj" >> $LOG
  # no point gating the rest: the fix for this one usually invalidates them
  if [ "$VERDICT" != GREEN ]; then echo "STOPPED at first failure" >> $LOG; break; fi
done
echo DONE >> $LOG
