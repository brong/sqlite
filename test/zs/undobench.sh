#!/bin/sh
# The savepoint undo log's cost, which no other benchmark here can see.
#
# zsbtWrite fetches a row's current version before overwriting it, so the log
# can put it back.  Nothing in zskvbench exercises that: a prepared single-row
# INSERT needs no statement journal, so no undo mark is ever open and the fetch
# never runs (verified by instrumenting the funnel: logged=0 on every store row).
#
# What DOES open a mark, measured the same way, is broader than savepoints:
#
#   INSERT ... SELECT   logs every row, no savepoint needed -- the statement
#                       journal opens the mark.  The fetch MISSES.
#   UPDATE              logs every row, no savepoint needed.  The fetch HITS.
#   DELETE FROM t       logs NOTHING without a savepoint (the truncate path),
#                       and every row with one.
#   any of them inside an explicit SAVEPOINT: logged either way.
#
# So a savepoint is not the trigger for most of this, and the sp/nosp pair below
# is NOT a measurement of the undo log -- for insert and update both arms log,
# which is exactly why it reads as "+3%" there.  It is kept because that null
# result is the finding: wrapping a statement in SAVEPOINT costs almost nothing
# beyond what the statement journal already spends.
#
# To price the undo fetch itself, patch zsbtWrite to skip it (pOld=0, nOld=0,
# zrc=ZS_NOTFOUND) and diff against this baseline.  That build is UNSOUND --
# rollback replays nothing -- so it is a measurement tool, not a flag.  Measured
# that way at 500k rows: INSERT...SELECT -17%, UPDATE -19%, insert-then-update
# -14%, DELETE-in-savepoint -38% of user CPU.
#
# USER CPU is the number to read, not wall clock: this measures work per row,
# and wall clock here is dominated by sys time in the writes.  zs_nosync is on
# for the same reason.  Reported as the MINIMUM over reps, the usual choice for
# a CPU-bound measurement with one-sided noise.  N must be large enough that
# user CPU exceeds ~0.5s, because /usr/bin/time resolves to 10ms.
set -eu

BLD=${1:-.}
N=${N:-500000}
V=${V:-100}
REPS=${REPS:-3}
SH="$BLD/sqlite3zs"
[ -x "$SH" ] || { echo "no $SH -- build sqlite3zs first" >&2; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/undobench.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The row source is a TABLE, built during setup, not a recursive CTE.  A CTE
# materialises into an ephemeral table, which in this engine is another zeroskip
# database going through the same write funnel -- instrumenting it showed 60000
# writes for a 20000-row insert, two thirds of them the CTE's own.  Reading a
# plain table writes nothing, so each shape measures what it claims to.
GEN="(WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<$N)
      SELECT i FROM c)"
SEED_SRC="CREATE TABLE src(a INTEGER PRIMARY KEY, b BLOB);
          INSERT INTO src(a,b) SELECT i, randomblob($V) FROM $GEN;"
INS="INSERT INTO t(a,b) SELECT a, b FROM src;"

URI_ARGS='zs_nosync=1&zs_sync_ms=100000'

# $1 shape  $2 needs-t-populated  $3 statements between BEGIN and COMMIT
run_shape() {
  name=$1; populated=$2; body=$3
  seed="$WORK/seed.$name"
  rm -rf "$seed"
  if [ "$populated" = yes ]; then
    "$SH" "file:$seed?$URI_ARGS" \
      "$SEED_SRC CREATE TABLE t(a INTEGER PRIMARY KEY, b BLOB); $INS" >/dev/null
  else
    "$SH" "file:$seed?$URI_ARGS" \
      "$SEED_SRC CREATE TABLE t(a INTEGER PRIMARY KEY, b BLOB);" >/dev/null
  fi

  for sp in nosp sp; do
    if [ "$sp" = sp ]; then
      sql="BEGIN; SAVEPOINT s; $body RELEASE s; COMMIT;"
    else
      sql="BEGIN; $body COMMIT;"
    fi
    best=""
    i=0
    while [ "$i" -lt "$REPS" ]; do
      i=$((i+1))
      db="$WORK/run"
      rm -rf "$db"; cp -R "$seed" "$db"
      u=$( { /usr/bin/time -p "$SH" "file:$db?$URI_ARGS" "$sql" >/dev/null; } \
             2>&1 | awk '/^user/{print $2}' )
      case "$best" in
        "") best=$u ;;
        *)  best=$(awk -v a="$best" -v b="$u" 'BEGIN{print (b<a)?b:a}') ;;
      esac
    done
    rm -rf "$db"
    printf '  %-10s %-5s user %ss\n' "$name" "$sp" "$best"
    eval "R_${name}_${sp}=$best"
  done
  a=$(eval echo "\$R_${name}_nosp"); b=$(eval echo "\$R_${name}_sp")
  awk -v n="$name" -v a="$a" -v b="$b" 'BEGIN{
    if (a+0==0) { printf "  %-10s savepoint adds: n/a (raise N)\n\n", n; exit }
    printf "  %-10s savepoint adds %+.0f%%\n\n", n, (b-a)/a*100
  }'
}

echo "undobench: $N rows, $V-byte values, min user CPU of $REPS, zs_nosync"
echo "(read the absolute numbers; see the header for what the sp/nosp pair does"
echo " and does not tell you)"
echo
run_shape insert  no  "$INS"
run_shape update  yes "UPDATE t SET b=randomblob($V);"
run_shape rewrite no  "$INS UPDATE t SET b=randomblob($V);"
run_shape delete  yes "DELETE FROM t;"
