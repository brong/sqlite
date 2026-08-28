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
#   ab       format 2 against format 4 through SQL, reads only.  ANSWERED on
#            2026-08-28: format 4 wins all 12 cells on both rows, fetch
#            +6..79% and scan +3..56%.  Keep running it as the regression
#            check, and note that the laptop had fetch OVERLAPPING in all 12 --
#            the laptop-to-ZFS transfer is untrustworthy in BOTH directions,
#            having hidden a real win here after inventing one for format 3.
#
#   attrib   what that scan win IS.  ANSWERED on 2026-08-28: on SCAN both
#            halves are real (verification +9..34% where it resolves, layout a
#            further +1..41%), so quoting format 4's scan gain against format 2
#            quotes roughly half read-path verification that no longer happens.
#            On FETCH it is ALL layout -- verification is overlap or +1..3%
#            everywhere while layout carries +5..28%, and +62..77% at WITHOUT
#            ROWID/400B/2M.  Upstream called this comparison unmakeable; it is,
#            from the format-4 side, but not from the format-2 side.
#
#   vacuum   compaction memory, and the per-lookup discriminator.  The first
#            production run found peak RSS 2-7% BELOW format 2 -- a rounding
#            error, not a bound -- with a 2M WITHOUT ROWID/400B VACUUM peaking
#            at 9.6GB.  Upstream fixed that at 9e1a2ac (D-29, both writers
#            stream), and the LAPTOP now shows -38% / -29%, still near 10GB at
#            the big cell.  RE-RUNNING THIS PHASE IS THE POINT OF THE NEXT RUN:
#            how much of the fix lands on ZFS, where the residual is file
#            mappings rather than allocation.  It stays the fetch discriminator
#            too -- a per-LOOKUP cost survives collapsing to one file, and the
#            first run showed the gap WIDENING to +99..103% when it did.
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
