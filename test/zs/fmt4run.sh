#!/bin/bash
# The format-4 production run: everything the laptop could not decide.
#
#   ./test/zs/fmt4run.sh /mnt/bench128k
#   ./test/zs/fmt4run.sh /mnt/bench4k /mnt/bench128k     (both, ~2x the time)
#
# Run it on a QUIET machine.  The laptop attempt at phase 2 was voided by a
# runaway Finder process -- the same arm and cell went 14.9s to 1834s for its
# fetch phase, and because the degradation arrived PART WAY THROUGH, the early
# cells looked healthy while the late ones were nonsense.  Both sub-scripts now
# gate on that, but a gate turns a wasted run into a known-wasted run; it does
# not give you the measurement.  Check nothing else is running first.
#
# THREE PHASES, three different questions:
#
#   ab       format 2 against format 4 through SQL, reads only.  This is what
#            upstream asked for.  The laptop said: fetch OVERLAPS in all 12
#            cells (their +1.6-2.4% is below what the VDBE lets us resolve),
#            scan is a clean format-4 win in all 12 and it GROWS WITH VALUE
#            SIZE.  The transfer from laptop to ZFS is precisely what proved
#            untrustworthy for format 3, which inverted here at 2M.
#
#   attrib   what that scan win IS.  A win proportional to value size is
#            equally a denser layout and the removal of per-record
#            verification -- and upstream's own note says verification "pulled
#            every value byte into cache that the scan would otherwise never
#            read".  Three arms split it.  Upstream called this comparison
#            unfinished and unmakeable; it is unmakeable from the format-4
#            side, but makeable from the format-2 side, which we still have.
#            THIS IS THE PHASE THAT IS OWED UPSTREAM.
#
#   vacuum   the compaction memory question that merge_memory used to own.
#            Format 4 streams its merge output (the pointer array is written
#            last), so the field is gone -- but it never bounded a merge's
#            MAPPED INPUTS, and those are what made a 2M VACUUM peak at
#            1.2-1.4GB.  zskvbench reports peak RSS itself, so a --vacuum arm
#            answers it directly.  Also the discriminator for any fetch result:
#            a per-LOOKUP cost survives collapsing the database to one file.
#
# Each phase can be run alone: PHASES=attrib ./test/zs/fmt4run.sh /mnt/bench128k
#
# Sizes are deliberately not shrunk.  Any scan or fetch ratio measured at 20k
# is a cache benchmark, 200k and 2M are the sizes that mean anything here, and
# 2M is the one that resolved the format-3 question when 500k could not.
set -u

OUT=${OUT:-/tmp/zsfmt4-$(date +%Y%m%d-%H%M%S)}
PHASES=${PHASES:-"ab attrib vacuum"}
PASSES=${PASSES:-8}
has_phase(){ case " $PHASES " in *" $1 "*) return 0;; *) return 1;; esac; }

[ -f ./Makefile ] || { echo "run from a configured build directory" >&2; exit 2; }
[ $# -ge 1 ] || { echo "usage: $0 DATASET_MOUNTPOINT [MORE...]" >&2; exit 2; }
for d in "$@"; do
  [ -d "$d" ] || { echo "no such dataset: $d" >&2; exit 2; }
  [ -w "$d" ] || { echo "not writable: $d" >&2; exit 2; }
done
mkdir -p "$OUT" || exit 2

# ---------------------------------------------------------------- environment
# READ THESE TWO LINES BEFORE THE NUMBERS.  A run that benchmarked an un-pushed
# commit looks exactly like a run that found nothing, and a dataset named for
# one recordsize and created with another has wasted a run here before.
{
  echo "commit:   $(git rev-parse HEAD 2>/dev/null)"
  # VENDOR is an append-only log, so it holds every past "source:" line too.
  # Without the head -1 this prints the whole vendoring history and the reader
  # cannot tell which commit is live.
  echo "vendored: $(sed -n 's/^source: //p' ext/zeroskip/VENDOR | head -1)"
  echo "modified: $(git status --porcelain src/ ext/zeroskip/ test/ | grep -c '^ M') tracked, $(git status --porcelain src/ ext/zeroskip/ test/ | grep -c '^??') untracked"
  uname -a
  for d in "$@"; do
    ds=$(df --output=source "$d" 2>/dev/null | tail -1)
    echo "-- $d  (dataset $ds)  recordsize $(zfs get -H -o value recordsize "$ds" 2>/dev/null || echo '?')"
  done
  echo "-- load at start:$(uptime)"
} > "$OUT/env.txt" 2>&1
cat "$OUT/env.txt"
echo
echo "output: $OUT"

# The whole run compares against a format-4 tree.  If this build does not write
# format 4, every phase below is comparing something else, so stop here rather
# than three guard failures deep.
echo "== checking this tree writes format 4 =="
rm -f ./*.o libsqlite3.a
make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
     lib zskvbench > "$OUT/build-check.log" 2>&1 \
  || { echo "BUILD FAILED, see $OUT/build-check.log" >&2; exit 1; }
CK="$1/fmt4check"; rm -rf "$CK"; mkdir -p "$CK"
./zskvbench --dir "$CK" -n 200000 --reps 1 --rowid --build-only >/dev/null 2>&1
f=$(find "$CK" -type f -name 'zeroskip-*' ! -name '*.current' | head -1)
dig=$(od -An -c -N16 "$f" 2>/dev/null | tr -s ' ' | sed 's/^ //' | awk '{print $10}')
rm -rf "$CK"
echo "  magic version digit: '$dig'"
[ "$dig" = 4 ] || { echo "STOP: this tree writes '$dig', not format 4." >&2
                    echo "  '2' is the withdrawn format 3 -- git pull." >&2; exit 1; }

fail=0
phase(){ echo; echo "############ phase: $1"; }

if has_phase ab; then
phase "ab -- format 2 against format 4, reads (the headline)"
SIZES="200000 2000000" VALS="100 200 400" SHAPES="rowid withoutrowid" \
  PASSES="$PASSES" OUT="$OUT/ab" ./test/zs/fmt2ab.sh "$@" \
  > "$OUT/ab.log" 2>&1 || fail=1
tail -40 "$OUT/ab/summary.txt" 2>/dev/null || tail -20 "$OUT/ab.log"
fi

if has_phase attrib; then
phase "attrib -- how much of the scan win is layout, how much is verification"
# Fewer cells than `ab` on purpose: three arms, and the value-size GRADIENT is
# the evidence, so the two ends of it carry the argument.
SIZES="200000 2000000" VALS="100 400" SHAPES="rowid withoutrowid" \
  PASSES="$PASSES" OUT="$OUT/attrib" ./test/zs/scanattrib.sh "$@" \
  > "$OUT/attrib.log" 2>&1 || fail=1
tail -50 "$OUT/attrib/summary.txt" 2>/dev/null || tail -20 "$OUT/attrib.log"
grep -q "RUN VOID" "$OUT/attrib.log" && { echo; grep -A6 "RUN VOID" "$OUT/attrib.log"; }
fi

if has_phase vacuum; then
phase "vacuum -- compaction memory, and the per-lookup discriminator"
# VACUUM=1 compacts between the build and the reads.  Two answers for the price
# of one: zskvbench's peak RSS says whether format 4 bounds a compaction (the
# question merge_memory used to own), and the file count per cell says whether
# any fetch difference is per-LOOKUP or per-matched-FILE.
VACUUM=1 SIZES="2000000" VALS="100 400" SHAPES="rowid withoutrowid" \
  PASSES="$PASSES" OUT="$OUT/vacuum" ./test/zs/fmt2ab.sh "$@" \
  > "$OUT/vacuum.log" 2>&1 || fail=1
tail -30 "$OUT/vacuum/summary.txt" 2>/dev/null || tail -20 "$OUT/vacuum.log"
echo
echo "-- peak RSS per arm (this is the merge_memory replacement question) --"
grep -h "peak RSS" "$OUT/vacuum/reads.txt" 2>/dev/null \
  | awk -F'|' '{print $2, $3, $4, $5, $6}' | sort -u | sed 's/^/  /'
fi

echo "-- load at end:$(uptime)" >> "$OUT/env.txt"
echo
echo "============================================================"
echo "everything is under $OUT"
echo "  env.txt            commit, recordsize, load at both ends"
echo "  ab/summary.txt     format 2 -> format 4, reads"
echo "  attrib/summary.txt the layout / verification split"
echo "  vacuum/summary.txt compacted, with peak RSS"
echo "send those four back; the reads.txt files are the raw rows."
[ $fail = 0 ] || echo "NOTE: at least one phase exited nonzero -- check its .log"
exit $fail
