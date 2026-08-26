#!/bin/bash
# Format 2 against format 3, on the same host, for the READ rows.
#
# This is the one comparison the matrix and the stock-vs-zeroskip runs cannot
# make: everything else measures us against the btree, which is a different
# question from whether key/value separation earned its place.  Upstream's
# +14% fetch / +37% scan were measured on a laptop and on their own fixture.
#
# THE ARM IS RECONSTRUCTED FROM THIS REPOSITORY'S HISTORY, not from a sibling
# checkout, so it runs on a production box that has only this tree.  Commit
# $FMT2 vendored library 2.9.1 (upstream ffbb7e1); upstream's zeroskip.c and
# zeroskip.h are byte-identical from there through 85ba990, the last commit
# before format 3's implementation landed in 8dc8893 -- so this IS the arm,
# and the intervening upstream commits are a spec and a design document.
#
# Its src/btree_zs.c comes from the same commit, because today's sets
# setup.merge_memory and 2.9.1's zs_open_data has no such field.  The two
# versions of that file differ ONLY by that field and by the removal of
# zs_nocsum: nothing either arm's read path touches.
#
# test/zskvbench.c is the CURRENT one in both arms, deliberately.  The version
# that shipped with the format-2 commit carried the (i * 7919) int overflow, so
# above 271181 records its fetch row times a hit/miss mixture -- which is
# exactly the size range this script exists to measure.  Holding the benchmark
# fixed and moving only the library is the whole point.
#
# THE GUARD IS ON THE ON-DISK ARTIFACT, NOT THE BINARY.  Two arms that differ
# only in their executables would satisfy a `cmp` and could still write the
# same format -- a null instrument that reads as a clean result.  So each arm
# writes a database, and the magic's version digit in an IN-ORDER file's header
# is read back: '1' for format 2, '2' for format 3.  If they do not differ, the
# script stops before measuring anything.
set -u
FMT2=${FMT2:-e2bba7b2e8}
N_SMALL=${N_SMALL:-200000}
N_BIG=${N_BIG:-2000000}
PASSES=${PASSES:-3}
OUT=${OUT:-/tmp/zsfmt2-$(date +%Y%m%d-%H%M%S)}

[ -f ./Makefile ] || { echo "run from a configured build directory" >&2; exit 2; }
[ $# -ge 1 ] || { echo "usage: $0 DATASET_MOUNTPOINT [MORE...]" >&2; exit 2; }
mkdir -p "$OUT" || exit 2
echo "output: $OUT"

SAVED="$OUT/saved"; mkdir -p "$SAVED"
cp ext/zeroskip/zeroskip.c ext/zeroskip/zeroskip.h src/btree_zs.c "$SAVED/"
restore(){ cp "$SAVED/zeroskip.c" "$SAVED/zeroskip.h" ext/zeroskip/ 2>/dev/null
           cp "$SAVED/btree_zs.c" src/ 2>/dev/null; }
trap 'restore' EXIT INT TERM        # a half-swapped tree is worse than no run


build(){  # build ARMNAME  -- sources must already be in place
  rm -f ./*.o libsqlite3.a zskvbench
  make USE_AMALGAMATION=0 \
       OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
       lib zskvbench > "$OUT/build-$1.log" 2>&1 \
    || { echo "BUILD FAILED ($1), see $OUT/build-$1.log" >&2; exit 1; }
  cp zskvbench "zskvbench.$1"
}

echo "== building the format-2 arm from $FMT2 =="
git show "$FMT2:ext/zeroskip/zeroskip.c" > ext/zeroskip/zeroskip.c || exit 1
git show "$FMT2:ext/zeroskip/zeroskip.h" > ext/zeroskip/zeroskip.h || exit 1
git show "$FMT2:src/btree_zs.c"          > src/btree_zs.c          || exit 1
build fmt2
echo "== building the format-3 arm from the working tree =="
restore
build fmt3

# ---- the guard: what did each arm actually WRITE? ----------------------------
# Every zeroskip file header opens with the 16-byte magic, whose byte 9 is the
# major format version as an ASCII digit.  An in-order file is the one that
# carries format 3's keys_len/values_len, so that is the one read.
fmtof(){  # fmtof BIN DIR -> prints the version digit and the magic
  local bin=$1 d=$2 f
  rm -rf "$d"; mkdir -p "$d"
  # --build-only, not --only 1000: bench_store deletes its database when it
  # finishes, so there is nothing left to inspect.  --build-only exists to
  # leave one behind, and 200k records is comfortably past the rollover, so
  # conversions have produced in-order files by the time it returns.
  "./$bin" --dir "$d" -n 200000 --reps 1 --rowid --build-only >/dev/null 2>&1
  f=$(find "$d" -type f -name 'zeroskip-*' ! -name '*.current' | head -1)
  [ -n "$f" ] || { echo "NO-INORDER-FILE"; return; }
  od -An -c -N16 "$f" | tr -s ' ' | sed 's/^ //'
}

G="$OUT/guard"
echo "== guard: reading the format each arm wrote to disk =="
m2=$(fmtof zskvbench.fmt2 "$G/a"); echo "  fmt2 arm magic: $m2"
m3=$(fmtof zskvbench.fmt3 "$G/b"); echo "  fmt3 arm magic: $m3"
rm -rf "$G"
d2=$(printf '%s' "$m2" | awk '{print $10}')
d3=$(printf '%s' "$m3" | awk '{print $10}')
echo "  version digit: fmt2='$d2'  fmt3='$d3'"
if [ "$d2" != "1" ] || [ "$d3" != "2" ] || [ "$d2" = "$d3" ]; then
  echo "GUARD FAILED: the two arms did not write different formats." >&2
  echo "  Expected the format-2 arm to write '1' and the format-3 arm '2'." >&2
  echo "  Measuring now would compare an arm against itself." >&2
  exit 1
fi
echo "  ok -- the arms differ in what they WROTE, not merely in their binaries"

# ---- the measurement --------------------------------------------------------
{
echo "commit:   $(git rev-parse HEAD)"
echo "fmt2 arm: $FMT2 ($(git show -s --format=%s "$FMT2" | cut -c1-60))"
echo "guard:    fmt2 magic digit '$d2', fmt3 magic digit '$d3'"
for d in "$@"; do
  ds=$(df --output=source "$d" 2>/dev/null | tail -1)
  echo "-- $d recordsize $(zfs get -H -o value recordsize "$ds" 2>/dev/null || echo '?')"
done
} | tee "$OUT/env.txt"

for d in "$@"; do
  for n in "$N_SMALL" "$N_BIG"; do
    for p in $(seq 1 "$PASSES"); do
      if [ $((p % 2)) -eq 1 ]; then order="fmt2 fmt3"; else order="fmt3 fmt2"; fi
      for a in $order; do
        w="$d/f2ab"; rm -rf "$w"; mkdir -p "$w"
        echo "-- $d n=$n pass=$p arm=$a"
        "./zskvbench.$a" --dir "$w" -n "$n" --reps 1 --rowid --only reads
        rm -rf "$w"
      done
    done
  done
done | tee "$OUT/reads.txt"

echo
echo "wrote $OUT/reads.txt"
