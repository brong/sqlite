#!/bin/bash
# Three-way key-value benchmark, suitable for production hardware:
#
#   1. raw zeroskip           (upstream zsbench, vendored sources)
#   2. SQLite on zeroskip     (zskvbench against the SQLITE_ZEROSKIP build)
#   3. stock SQLite btree     (same zskvbench source against the
#                              amalgamation; default journal AND WAL)
#
# All four runs use identical workloads: "key%08d" keys, fixed-size
# values, a 1/10/100/1000-records-per-transaction store sweep,
# pseudorandom point fetches, and a full scan.
#
# Usage, from a configured build directory of this tree:
#
#   ./test/zs/kvbench-all.sh [--dir PATH] [-n N] [--value N] [--reps N]
#
# On production hardware pass --dir on the filesystem you care about;
# every run creates and removes its own subdirectories there.  The
# durability-bound rows (1 and 10 per txn) are where storage hardware
# shows up; the 1000-per-txn row is the CPU-bound engine comparison.
set -eu

if [ ! -f Makefile ]; then
  echo "run from a configured build directory (./configure first)" >&2
  exit 2
fi

# The library objects are shared between the two engine configurations
# and make cannot see that OPTIONS changed, so a directory that ever
# built the stock library would link zskvbench against the BTREE and
# report it as zeroskip.  Always start the zeroskip build from clean.
echo "building (three configurations)..." >&2
rm -f *.o libsqlite3.a
make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
     lib zskvbench zsbench >/dev/null
make zskvbenchstock >/dev/null

# ...and verify it, because a silent engine mix-up produces numbers that
# look reasonable rather than obviously broken.
eng=$(./zskvbench --dir "${TMPDIR:-/tmp}" -n 1 --reps 1 2>/dev/null \
      | sed -n 's/.*engine=\([a-z]*\).*/\1/p' | head -1)
if [ "$eng" != "zeroskip" ]; then
  echo "ERROR: zskvbench linked the '$eng' engine, not zeroskip." >&2
  echo "       run 'rm -f *.o libsqlite3.a' and try again." >&2
  exit 1
fi
eng=$(./zskvbenchstock --dir "${TMPDIR:-/tmp}" -n 1 --reps 1 2>/dev/null \
      | sed -n 's/.*engine=\([a-z]*\).*/\1/p' | head -1)
if [ "$eng" != "btree" ]; then
  echo "ERROR: zskvbenchstock linked the '$eng' engine, not btree." >&2
  exit 1
fi

TOP="$(sed -n 's/^TOP *= *//p' Makefile | head -1)"
VEND="$(sed -n 's/^source: //p' "$TOP/ext/zeroskip/VENDOR" 2>/dev/null | cut -c1-12)"

# each tool gets its own subdirectory: zsbench cleans its working area
# aggressively, so nothing may share it
BASEDIR="${TMPDIR:-/tmp}"
declare -a PASSARGS=()
declare -a SQLARGS=()          # zsbench does not know these
while [ $# -gt 0 ]; do
  if [ "$1" = "--dir" ] && [ $# -gt 1 ]; then
    BASEDIR="$2"; shift 2
  elif [ "$1" = "--rowid" ]; then
    # INTEGER PRIMARY KEY instead of WITHOUT ROWID/TEXT: the commoner
    # SQLite shape, and the one whose insert path goes through
    # OP_NotExists/TableMoveto rather than OP_NewRowid.  The two paths
    # are disjoint, so an optimisation on one measures as free on the
    # other -- run both shapes before concluding anything.
    SQLARGS+=("$1"); shift
  else
    PASSARGS+=("$1"); shift
  fi
done
mkdir -p "$BASEDIR/kvb-raw" "$BASEDIR/kvb-zs" "$BASEDIR/kvb-stock"

echo
echo "=== 1. raw zeroskip (zsbench, vendored @ ${VEND:-?}) ==="
./zsbench ${PASSARGS[@]+"${PASSARGS[@]}"} --dir "$BASEDIR/kvb-raw"

echo
echo "=== 2. SQLite on zeroskip ==="
./zskvbench ${PASSARGS[@]+"${PASSARGS[@]}"} ${SQLARGS[@]+"${SQLARGS[@]}"} \
  --dir "$BASEDIR/kvb-zs"

echo
echo "=== 2b. SQLite on zeroskip (nosync, 1s periodic sync) ==="
./zskvbench ${PASSARGS[@]+"${PASSARGS[@]}"} ${SQLARGS[@]+"${SQLARGS[@]}"} \
  --dir "$BASEDIR/kvb-zs" --uri "zs_nosync=1&zs_sync_ms=1000"

echo
echo "=== 3a. stock SQLite btree (default journal) ==="
./zskvbenchstock ${PASSARGS[@]+"${PASSARGS[@]}"} ${SQLARGS[@]+"${SQLARGS[@]}"} \
  --dir "$BASEDIR/kvb-stock"

echo
echo "=== 3b. stock SQLite btree (WAL, synchronous=NORMAL) ==="
./zskvbenchstock ${PASSARGS[@]+"${PASSARGS[@]}"} ${SQLARGS[@]+"${SQLARGS[@]}"} \
  --dir "$BASEDIR/kvb-stock" \
  --init "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"

rm -rf "$BASEDIR/kvb-raw" "$BASEDIR/kvb-zs" "$BASEDIR/kvb-stock"
