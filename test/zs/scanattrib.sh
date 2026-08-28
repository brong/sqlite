#!/bin/bash
# How much of format 4's scan win is the LAYOUT, and how much is the
# verification it removed?
#
# fmt2ab.sh answers "format 2 against format 4" and finds a large, one-way scan
# win that GROWS WITH VALUE SIZE.  That gradient is the problem: it is equally
# what a denser layout would look like and what removing per-record
# verification would look like, and upstream says which it expects --
# verification "pulled every value byte into cache that the scan would
# otherwise never read.  The cost was memory traffic, not hashing."  A cost
# proportional to value bytes touched produces exactly this gradient.  So the
# headline number is UNATTRIBUTED until verification is taken off both sides.
#
# UPSTREAM CALLED THIS COMPARISON UNFINISHED AND UNMAKEABLE.  It is unmakeable
# from the format-4 side: no record carries a checksum, so there is nothing to
# switch off, and ZS_NOCSUM is rejected outright.  It IS makeable from the
# FORMAT-2 side, which is available to us because we keep the old arm in
# history: library 2.9.1 still has ZS_NOCSUM and the btree_zs.c of that commit
# still wires `zs_nocsum=1` to it.  Three arms rather than two:
#
#   fmt2     format 2, verifying          -- what we used to ship
#   fmt2nc   format 2, zs_nocsum=1        -- format 2's layout, no verification
#   fmt4     format 4                     -- cannot verify by construction
#
#   fmt2   -> fmt2nc   what verification cost, at THIS value size
#   fmt2nc -> fmt4     THE LAYOUT DIFFERENCE, verification off both sides
#   fmt2   -> fmt4     the end-to-end number fmt2ab.sh reports
#
# The middle row is the one this script exists for.  If fmt2nc -> fmt4 is a
# wash, format 4's scan headline is the removal of a safety net being cashed in
# as a speed win, and should be described that way rather than as a better
# layout.
#
# THE FMT2 ARM TAKES ITS OWN src/btree_zs.c HERE, unlike fmt2ab.sh, which now
# shares the current one.  It has to: the current file has no zs_nocsum
# plumbing at all, so the middle arm would be silently identical to the first.
# That makes the engine a second difference between fmt2* and fmt4 -- accepted
# deliberately, because the fmt2->fmt2nc and fmt2nc->fmt4 comparisons that
# matter each hold the engine fixed within themselves, and the two versions of
# that file differ only in comments and the nocsum plumbing anyway.
#
# THE GUARD IS BEHAVIOURAL, NOT TEXTUAL.  A URI parameter that is parsed but
# reaches nothing, or one the library ignores, would make fmt2nc a copy of fmt2
# and this script would report "verification cost nothing" -- a null instrument
# reading as a finding.  So before measuring anything: build a format-2
# database, corrupt one value byte INSIDE AN IN-ORDER FILE (the active file's
# span checksum is verified either way, so corrupting that proves nothing), and
# require that the verifying arm reports SQLITE_CORRUPT while the nocsum arm
# reads straight through it.  If both agree, the script stops.
#
#   SIZES="200000 2000000"    record counts
#   VALS="100 400"            value sizes -- the gradient IS the evidence
#   SHAPES="rowid withoutrowid"
#   PASSES=6                  arm order rotates every pass
set -u
FMT2=${FMT2:-e2bba7b2e8}
SIZES=${SIZES:-"200000 2000000"}
VALS=${VALS:-"100 400"}
SHAPES=${SHAPES:-"rowid withoutrowid"}
PASSES=${PASSES:-6}
OUT=${OUT:-/tmp/zsscanattrib-$(date +%Y%m%d-%H%M%S)}

[ -f ./Makefile ] || { echo "run from a configured build directory" >&2; exit 2; }
[ $# -ge 1 ] || { echo "usage: $0 DATASET_MOUNTPOINT [MORE...]" >&2; exit 2; }
mkdir -p "$OUT" || exit 2
echo "output: $OUT"

SAVED="$OUT/saved"; mkdir -p "$SAVED"
cp ext/zeroskip/zeroskip.c ext/zeroskip/zeroskip.h src/btree_zs.c "$SAVED/"
restore(){ cp "$SAVED/zeroskip.c" "$SAVED/zeroskip.h" ext/zeroskip/ 2>/dev/null
           cp "$SAVED/btree_zs.c" src/ 2>/dev/null; }
trap 'restore' EXIT INT TERM        # a half-swapped tree is worse than no run

build(){  # build ARMNAME -- sources must already be in place
  rm -f ./*.o libsqlite3.a zskvbench
  make USE_AMALGAMATION=0 \
       OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
       lib zskvbench sqlite3zs > "$OUT/build-$1.log" 2>&1 \
    || { echo "BUILD FAILED ($1), see $OUT/build-$1.log" >&2; exit 1; }
  cp zskvbench "zskvbench.$1"; cp sqlite3zs "sqlite3zs.$1"
}

echo "== building the format-2 arm from $FMT2 (with its own btree_zs.c) =="
git show "$FMT2:ext/zeroskip/zeroskip.c" > ext/zeroskip/zeroskip.c || exit 1
git show "$FMT2:ext/zeroskip/zeroskip.h" > ext/zeroskip/zeroskip.h || exit 1
git show "$FMT2:src/btree_zs.c"          > src/btree_zs.c          || exit 1
build fmt2
echo "== building the format-4 arm from the working tree =="
restore
build fmt4

# ---- guard 1: what did each arm WRITE? --------------------------------------
fmtof(){  # fmtof BIN DIR -> the 16 magic bytes of an IN-ORDER file
  local bin=$1 d=$2 f
  rm -rf "$d"; mkdir -p "$d"
  "./$bin" --dir "$d" -n 200000 --reps 1 --rowid --build-only >/dev/null 2>&1
  f=$(find "$d" -type f -name 'zeroskip-*' ! -name '*.current' | head -1)
  [ -n "$f" ] || { echo "NO-INORDER-FILE"; return; }
  od -An -c -N16 "$f" | tr -s ' ' | sed 's/^ //'
}
G="$OUT/guard"
echo "== guard 1: the format each arm wrote to disk =="
m2=$(fmtof zskvbench.fmt2 "$G/a"); m4=$(fmtof zskvbench.fmt4 "$G/b")
d2=$(printf '%s' "$m2" | awk '{print $10}')
d4=$(printf '%s' "$m4" | awk '{print $10}')
echo "  version digit: fmt2='$d2'  fmt4='$d4'"
if [ "$d2" != "1" ] || [ "$d4" != "4" ]; then
  echo "GUARD FAILED: expected '1' and '4'.  A '2' is the withdrawn format 3" >&2
  echo "  and means a stale working tree.  Measuring now compares nothing." >&2
  exit 1
fi

# ---- guard 2: is zs_nocsum LIVE, or merely accepted? ------------------------
# Corrupt one value byte inside an IN-ORDER file.  The active file's span
# checksum is verified with or without the flag (F-5e), so corrupting that
# would fail both arms and prove nothing about the flag.
echo "== guard 2: zs_nocsum must change what the engine DOES =="
C="$G/c"; rm -rf "$C"; mkdir -p "$C"
./sqlite3zs.fmt2 "$C/db" "CREATE TABLE kv(k INTEGER PRIMARY KEY, v TEXT);
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<50000)
INSERT INTO kv SELECT i, printf('%.100c', 'Z') FROM c;" >/dev/null 2>&1
ino=$(find "$C/db" -type f -name 'zeroskip-*' ! -name '*.current' | head -1)
[ -n "$ino" ] || { echo "GUARD FAILED: no in-order file to corrupt" >&2; exit 1; }
off=$(python3 -c "d=open('$ino','rb').read(); print(d.find(b'ZZZZZZZZZZZZZZZZ', 200))")
[ "$off" -gt 0 ] 2>/dev/null || { echo "GUARD FAILED: no value bytes found" >&2; exit 1; }
python3 -c "p='$ino'; d=bytearray(open(p,'rb').read()); d[$off]=ord('Q'); open(p,'wb').write(d)"
q="SELECT count(*) FROM kv;"
ver=$(./sqlite3zs.fmt2 "file:$C/db"              "$q" 2>&1)
noc=$(./sqlite3zs.fmt2 "file:$C/db?zs_nocsum=1"  "$q" 2>&1)
echo "  verifying arm: $ver"
echo "  nocsum arm:    $noc"
case "$ver" in *malformed*) ;; *)
  echo "GUARD FAILED: the verifying arm did not reject a corrupt record." >&2
  echo "  Then fmt2 and fmt2nc are the same configuration and the middle" >&2
  echo "  row of this table would be a null instrument." >&2; exit 1;; esac
case "$noc" in 50000) ;; *)
  echo "GUARD FAILED: the nocsum arm did not read through the corruption." >&2
  echo "  zs_nocsum=1 is not reaching the library." >&2; exit 1;; esac
rm -rf "$G"
echo "  ok -- verification is on in one arm and off in the other"

# ---- guard 3: was the MACHINE quiet for the whole run? ----------------------
# The first attempt at this measurement was voided by a runaway Finder process
# on the laptop: the same arm and cell went 14.9s to 1834s for its fetch phase,
# and the degradation came on PART WAY THROUGH, so the early cells looked
# healthy and the late ones were nonsense.  Arm rotation does not save you from
# that -- it bounds a monotonic drift, not a 100x one.
#
# So a fixed cheap cell is measured before and after the sweep, and the run is
# VOID if they disagree.  A machine that was quiet at both ends and loud in the
# middle would still slip through, which is what the per-arm spread check in
# the summary is for.
refcell(){ # -> scan rate of one fixed cell
  local d=$1
  rm -rf "$d"; mkdir -p "$d"
  ./zskvbench.fmt4 --dir "$d" -n 200000 --value 100 --reps 1 --rowid --only reads \
    2>/dev/null | awk '/^  scan/ {print $2+0}'
  rm -rf "$d"
}
REFDIR="$1/scattr-ref"
echo "== guard 3: reference cell before the sweep =="
REF0=$(refcell "$REFDIR")
echo "  reference scan rate: $REF0 rows/s"
[ -n "$REF0" ] || { echo "GUARD FAILED: no reference reading" >&2; exit 1; }

# ---- the measurement --------------------------------------------------------
{
echo "commit:   $(git rev-parse HEAD)"
echo "fmt2 arm: $FMT2 ($(git show -s --format=%s "$FMT2" | cut -c1-60))"
echo "guard:    fmt2 digit '$d2', fmt4 digit '$d4'; nocsum verified behavioural"
echo "sweep:    sizes=[$SIZES] values=[$VALS] shapes=[$SHAPES] passes=$PASSES"
for d in "$@"; do
  ds=$(df --output=source "$d" 2>/dev/null | tail -1)
  echo "-- $d recordsize $(zfs get -H -o value recordsize "$ds" 2>/dev/null || echo '?')"
done
} | tee "$OUT/env.txt"

run_arm(){  # run_arm ARM DIR N VAL SHAPEFLAG
  case $1 in
    fmt2)   ./zskvbench.fmt2 --dir "$2" -n "$3" --value "$4" --reps 1 $5 --only reads 2>&1 ;;
    fmt2nc) ./zskvbench.fmt2 --dir "$2" -n "$3" --value "$4" --reps 1 $5 --only reads \
              --uri "zs_nocsum=1" 2>&1 ;;
    fmt4)   ./zskvbench.fmt4 --dir "$2" -n "$3" --value "$4" --reps 1 $5 --only reads 2>&1 ;;
  esac
}

RAW="$OUT/reads.txt"; : > "$RAW"
for d in "$@"; do
  rs=$(basename "$d")
  for sh in $SHAPES; do
    if [ "$sh" = rowid ]; then shflag=--rowid; else shflag=; fi
    for v in $VALS; do
      for n in $SIZES; do
        for p in $(seq 1 "$PASSES"); do
          # rotate all three, so no arm sits in a fixed slot
          case $((p % 3)) in
            1) order="fmt2 fmt2nc fmt4" ;;
            2) order="fmt2nc fmt4 fmt2" ;;
            0) order="fmt4 fmt2 fmt2nc" ;;
          esac
          for a in $order; do
            w="$d/scattr"; rm -rf "$w"; mkdir -p "$w"
            run_arm "$a" "$w" "$n" "$v" "$shflag" | sed "s/^/$rs|$sh|$v|$n|$a|/"
            rm -rf "$w"
          done
        done
        echo "  done $rs $sh value=$v n=$n" >&2
      done
    done
  done
done | tee -a "$RAW"

echo "== guard 3: reference cell after the sweep =="
REF1=$(refcell "$REFDIR")
echo "  before: $REF0 rows/s   after: $REF1 rows/s"
DRIFT=$(python3 -c "
a,b=float('$REF0'),float('$REF1' or 0)
print('%.1f' % (abs(b/a-1)*100 if a else 999))")
echo "  drift: ${DRIFT}%"
{ echo "reference: before $REF0, after $REF1, drift ${DRIFT}%"; } >> "$OUT/env.txt"
VOID=0
python3 -c "import sys; sys.exit(0 if float('$DRIFT')<=10 else 1)" || VOID=1
if [ $VOID = 1 ]; then
  echo
  echo "############################################################"
  echo "## RUN VOID: the machine was not quiet."
  echo "## The reference cell moved ${DRIFT}% between the start and"
  echo "## the end of the sweep ($REF0 -> $REF1 rows/s).  A few-percent"
  echo "## format difference cannot be read through that.  The raw"
  echo "## rows are in $RAW if you want to look, but do not quote a"
  echo "## number from them.  Find what else was running and re-run."
  echo "############################################################"
fi

python3 - "$RAW" <<'PYEOF' | tee "$OUT/summary.txt"
import re, sys, collections
V = collections.defaultdict(lambda: collections.defaultdict(list))
row = re.compile(r'^([^|]*)\|([^|]*)\|([^|]*)\|([^|]*)\|([^|]*)\|\s+(fetch|scan)\s+([\d.]+)/s')
for l in open(sys.argv[1]):
    m = row.match(l)
    if m:
        rs,sh,v,n,arm,what,rate = m.groups()
        V[(rs,sh,int(v),int(n),what)][arm].append(float(rate))

def cmp2(a,b):
    if not a or not b: return "no data"
    if min(b) > max(a): return "+%.0f..%.0f%%" % ((min(b)/max(a)-1)*100,(max(b)/min(a)-1)*100)
    if max(b) < min(a): return "%.0f..%.0f%%"  % ((max(b)/min(a)-1)*100,(min(b)/max(a)-1)*100)
    return "overlap"

print()
print("Attributing the format-2 -> format-4 read difference.")
print("Ranges over all passes, arm order rotated.  A verdict is called ONLY")
print("where two ranges do not overlap.\n")
print("  fmt2   format 2, verifying        fmt2nc  format 2, zs_nocsum=1")
print("  fmt4   format 4, cannot verify\n")
print("  verify   = fmt2 -> fmt2nc    what verification cost")
print("  LAYOUT   = fmt2nc -> fmt4    the format difference, verification off both")
print("  endtoend = fmt2 -> fmt4      what fmt2ab.sh reports\n")
# An arm whose OWN passes disagree by more than this is not measuring the
# format, it is measuring whatever else the machine was doing.  A cell like
# that gets no verdict at all -- printing "overlap" for it would read as
# "no difference found", which is a much stronger claim than "no signal".
UNSTABLE = 1.5

for rs,sh,v,n in sorted({(k[0],k[1],k[2],k[3]) for k in V}):
    print("=== %s  %s  value=%dB  n=%d ===" % (rs,sh,v,n))
    for what in ("scan","fetch"):
        d = V[(rs,sh,v,n,what)]
        a,b,c = d.get('fmt2',[]), d.get('fmt2nc',[]), d.get('fmt4',[])
        if not a or not b or not c: continue
        print("  %-5s fmt2 %9.0f-%-9.0f fmt2nc %9.0f-%-9.0f fmt4 %9.0f-%-9.0f"
              % (what, min(a),max(a), min(b),max(b), min(c),max(c)))
        worst = max(max(x)/min(x) for x in (a,b,c) if min(x) > 0)
        if worst > UNSTABLE:
            print("        NO VERDICT -- an arm's own passes spread %.1fx (>%.1fx)."
                  % (worst, UNSTABLE))
            print("        That is the machine, not the format.")
        else:
            print("        verify %-14s LAYOUT %-14s endtoend %s"
                  % (cmp2(a,b), cmp2(b,c), cmp2(a,c)))
    print()
PYEOF
echo
echo "raw:     $RAW"
echo "summary: $OUT/summary.txt"
exit $VOID
