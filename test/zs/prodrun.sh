#!/bin/bash
# One production measurement run, self-verifying, for a quiesced machine.
#
#   ./test/zs/prodrun.sh /mnt/bench4k /mnt/bench128k
#
# Three phases, because they answer different questions:
#
#   matrix    kvbench-all across both table shapes on each dataset.  This is
#             the reference table, and the only thing that can re-derive how
#             far SQL-on-zeroskip is from the stock btree.
#   cascade   the repack/conversion counters (A-17) at record counts where
#             the cascade actually runs.  At 20k records a bulk load rewrites
#             0.0x what it stored, at 1M it is 4.3x, so the matrix cannot see
#             this at all and it needs its own phase.
#   syscalls  strace counts and a perf profile of one bulk store.  Upstream
#             has asked twice whether the flat file-lifecycle group (open,
#             readdir, rename, unlink -- the cascade's, not per-commit) costs
#             real time on ZFS, where those calls resolve paths and dirty
#             directories.  Only this machine can answer it: on APFS all
#             three candidate fixes are worth ~nothing.
#
# Everything lands in one output directory, and the script stops before a long
# phase if a tool cannot do what the phase needs.  Two papercuts from the last
# run are checked for directly: zsbench takes a POSITIONAL filter and not
# --only, and a stale benchmark binary from a previous build silently measures
# the wrong thing.
set -u

OUT=${OUT:-/tmp/zsprod-$(date +%Y%m%d-%H%M%S)}
N=${N:-20000}
REPS=${REPS:-5}
NBIG=${NBIG:-2000000}
NMID=${NMID:-200000}
# Where a pointer-table cache lives.  Cyrus will run with this on tmpfs, a third
# configuration: no pool I/O to write the table and no pool read at open, at the
# price of being volatile -- after a reboot the first opener of each database
# rebuilds it, which is sound because a table is only ever an optimisation
# (every rejection is ZS_NOTFOUND, never an error).
IDXDIR=${IDXDIR:-/tmpfs/zsidx}
# PHASES lets a phase be re-run on its own -- phase 3 in particular, which
# needs privilege and so wants an interactive shell for sudo.
# mergemem was here and is DELETED, not disabled: format 4 removed the
# merge_memory field, so zs_merge_memory is now an unrecognised URI parameter
# and SQLite ignores it silently.  Left in the list the phase would have run
# three IDENTICAL arms and printed three overlapping ranges -- a null
# instrument reading as a clean null result, which is the exact failure mode
# this file has been bitten by before.
PHASES=${PHASES:-matrix cascade cached opens latency syscalls fsync}
has_phase(){ case " $PHASES " in *" $1 "*) return 0;; *) return 1;; esac; }

[ $# -ge 1 ] || { echo "usage: $0 DATASET_MOUNTPOINT [MORE...]" >&2; exit 2; }
[ -f ./Makefile ] || { echo "run from a configured build directory" >&2; exit 2; }
mkdir -p "$OUT" || exit 2
echo "output: $OUT"

say(){ echo; echo "=== $*"; }

# Shared by every phase, so a phase run on its own behaves like one run in
# sequence.  SUDO is set only when strace needs it AND sudo works without
# prompting -- never flail at a password prompt inside a long script.
SUDO=""
STRACE_OK=0
cleanup(){ rm -rf "$1" 2>/dev/null || ${SUDO:-} rm -rf "$1"; }
# A dataset's REAL recordsize, not the one its mountpoint is named for.  A run
# was wasted on /mnt/bench128k created at 4K, and the fsync phase's per-block
# reading is meaningless without it.  Prints "?" off ZFS rather than failing.
dataset_recordsize(){
  ds=$(df --output=source "$1" 2>/dev/null | tail -1)
  zfs get -H -o value recordsize "$ds" 2>/dev/null || echo '?'
}

probe_strace(){
  [ $STRACE_OK -eq 1 ] && return 0
  if ! command -v strace >/dev/null 2>&1; then
    echo "  strace is not installed (apt install strace)"
  elif strace -c -o /dev/null /bin/true >/dev/null 2>&1; then
    STRACE_OK=1; echo "  strace works unprivileged"
  elif sudo -n true >/dev/null 2>&1; then
    SUDO="sudo"; STRACE_OK=1; echo "  using cached sudo credentials"
  else
    echo "  strace needs privilege and sudo cannot prompt from here.  Either:"
    echo "    sudo sysctl -w kernel.yama.ptrace_scope=0"
    echo "    sudo sysctl -w kernel.perf_event_paranoid=1"
    echo "    OUT=$OUT PHASES='$PHASES' $0 $*"
    echo "  or:  sudo -v && OUT=$OUT PHASES='$PHASES' $0 $*"
  fi
  return 0
}

# ---------------------------------------------------------------- environment
say "environment"
{
  echo "commit:  $(git rev-parse HEAD 2>/dev/null)"
  echo "vendored: $(sed -n 's/^source: //p' ext/zeroskip/VENDOR)"
  uname -a
  lscpu 2>/dev/null | head -20
  free -g 2>/dev/null
  for d in "$@"; do
    ds=$(df --output=source "$d" 2>/dev/null | tail -1)
    echo "-- $d  (dataset $ds)"
    zfs get -o property,value \
      recordsize,compression,sync,logbias,primarycache,atime "$ds" 2>/dev/null
  done
  zpool status 2>/dev/null
} > "$OUT/env.txt" 2>&1
grep -E "^commit|^vendored|recordsize" "$OUT/env.txt" | head -20
echo "(read the recordsize lines above before trusting any label: a dataset"
echo " named for one recordsize and created with another has happened here)"

# ------------------------------------------------------------------- rebuild
# Never inherit binaries from a previous build: the library objects are shared
# between the two engine configurations and make cannot see that OPTIONS
# changed, so a stale libsqlite3.a links the BTREE into a binary that reports
# itself as zeroskip.
say "building (from clean)"
rm -f ./*.o libsqlite3.a
make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
     lib zskvbench zsbench > "$OUT/build.log" 2>&1 || {
  echo "BUILD FAILED, see $OUT/build.log" >&2; exit 1; }
make zskvbenchstock >> "$OUT/build.log" 2>&1 || {
  echo "BUILD FAILED (stock), see $OUT/build.log" >&2; exit 1; }
ls -l zskvbench zskvbenchstock zsbench | sed 's/^/  /'

# --------------------------------------------------------- capability checks
say "checking each tool does what the phases need"
CHK="$OUT/checkdir"; ok=1
probe(){ # label, command...
  local label=$1; shift
  rm -rf "$CHK"; mkdir -p "$CHK"
  if "$@" >/dev/null 2>&1; then
    echo "  ok    $label"
  else
    echo "  FAIL  $label -- $*"; ok=0
  fi
}
probe "zskvbench --only"      ./zskvbench      --dir "$CHK" -n 1000 --reps 1 --only 1000
probe "zskvbenchstock --only" ./zskvbenchstock --dir "$CHK" -n 1000 --reps 1 --only 1000
# zsbench filters POSITIONALLY.  --only is a usage error there, and that is
# what stopped the last run, so check the form actually used below.
probe "zsbench filter"        ./zsbench        --path "$CHK" -n 1000 --reps 1 "1000 per txn"
rm -rf "$CHK"
[ $ok -eq 1 ] || { echo "stopping: fix the above before the long phases" >&2; exit 1; }

# --------------------------------------------------------------------- matrix
if has_phase matrix; then
say "phase 1/3: matrix (this is the long one)"
for d in "$@"; do
  for shape in "" "--rowid"; do
    tag=$(basename "$d")${shape:+-rowid}
    echo "  $tag ..."
    ./test/zs/kvbench-all.sh --dir "$d" -n "$N" --reps "$REPS" $shape \
      > "$OUT/matrix-$tag.txt" 2>&1
    grep -E "per txn|fetch|scan|engine=|rewritten" "$OUT/matrix-$tag.txt" \
      | head -4 | sed 's/^/    /'
  done
done
fi

# -------------------------------------------------------------------- cascade
if has_phase cascade; then
say "phase 2/3: repack cascade and write amplification"
for d in "$@"; do
  for n in "$NMID" "$NBIG"; do
    w="$d/casc"; rm -rf "$w"; mkdir -p "$w"
    echo "-- $d n=$n SQL, cascade armed (default)"
    ./zskvbench --dir "$w" -n "$n" --reps 3 --rowid --only 1000
    rm -rf "$w"; mkdir -p "$w"
    # The same load with the cascade OFF the write path, then finished from
    # idle time.  Deferring removes about three quarters of the rewriting
    # rather than moving it -- the ladder re-merges the same bytes ~3x on the
    # way up -- so the totals decide it, and on a filesystem where a
    # page-cache write is nearly free they come out level while here they
    # should not.  This is the number upstream asked for.
    echo "-- $d n=$n SQL, cascade deferred + catch-up"
    ./zskvbench --dir "$w" -n "$n" --reps 3 --rowid --only 1000 --defer-repack
    rm -rf "$w"; mkdir -p "$w"
    echo "-- $d n=$n raw"
    ./zsbench --path "$w" -n "$n" --reps 3 "1000 per txn"
    rm -rf "$w"
    # rollover_size sweep, cascade ARMED.  A bigger generation cuts both
    # per-file axes at once (fewer files, shallower ladder) and upstream
    # measures 2MB -> 16MB taking 269 unlinks and 796MB of merging to 33 and
    # 449MB.  For four libraries this sweep reported the largest generation as
    # the WORST setting while rewriting a quarter of the bytes, which upstream
    # root-caused in 2.8.0 to a quadratic in the active file's private index
    # (a merge per fixed 1024 inserts, each O(nbase) with a record decode per
    # comparison).  The bound is proportional now and 64MB went from 0.71x of
    # the default to 1.33x on the laptop, so this sweep is a RETEST, not a
    # confirmation.  With the pointer table too, because a larger generation
    # makes snapshot open linear in a bigger replay and our writers are
    # short-lived processes.
    #
    # And in both arrival orders.  Every store number this project has ever
    # produced used ascending keys, because a rowid table's keys arrive
    # sorted; a secondary index's do not, and --random is 7-8x slower on the
    # laptop at every rollover size.  It is also the order that made the
    # quadratic loud (upstream: 2.04s of merging ascending, 6.45s random), so
    # a sweep in one order can miss the thing it is sweeping for.
    if [ "$n" = "$NBIG" ]; then
      for mb in 2 16 64; do
        for idx in "" "&zs_index=local"; do
          w="$d/roll"; rm -rf "$w"; mkdir -p "$w"
          echo "-- $d n=$n rollover=${mb}MB${idx:+ +ptrtable} armed"
          ./zskvbench --dir "$w" -n "$n" --reps 3 --rowid --only 1000 \
            --uri "zs_rollover=$((mb*1024*1024))$idx"
          rm -rf "$w"
        done
        w="$d/roll"; rm -rf "$w"; mkdir -p "$w"
        echo "-- $d n=$n rollover=${mb}MB armed, RANDOM keys"
        ./zskvbench --dir "$w" -n "$n" --reps 3 --rowid --only 1000 --random \
          --uri "zs_rollover=$((mb*1024*1024))"
        rm -rf "$w"
      done
      # and the best-of-both question: deferred on top of a large generation
      w="$d/roll"; rm -rf "$w"; mkdir -p "$w"
      echo "-- $d n=$n rollover=16MB deferred + catch-up"
      ./zskvbench --dir "$w" -n "$n" --reps 3 --rowid --only 1000 \
        --defer-repack --uri "zs_rollover=$((16*1024*1024))"
      rm -rf "$w"
    fi
  done
done > "$OUT/cascade.txt" 2>&1
grep -E "^--|per txn|rewritten|deferred" "$OUT/cascade.txt" | sed 's/^/  /'
fi

# ---------------------------------------------------------- (no mergemem)
# The merge_memory sweep lived here and is gone with the field.  What it
# answered is worth carrying forward as a fact rather than a phase: on ZFS the
# knob did NOTHING -- every arm overlapped at both recordsizes and both sizes,
# because a merge there is I/O-bound (4.3-4.5 ms/MB of repack against ~0.5 on
# APFS) and the CPU-side difference is swamped.  So the deployment answer was
# "stream, the memory is free", and format 4 makes that unconditional: the
# pointer array is written after the records, so a merge holds 8 bytes per
# record in one pass and there is no output to bound.
#
# WHAT REPLACES IT IS A DIFFERENT QUESTION, and it is open.  merge_memory never
# bounded a merge's MAPPED INPUTS, and those are what made VACUUM peak at
# 1.2-1.4GB RSS at 2M records.  Format 4 does not obviously change that.  Until
# someone measures a VACUUM's peak RSS on production under format 4, the
# compaction memory figure in the doc is a format-3 number.

# --------------------------------------------------------------------- cached
if has_phase cached; then
say "phase: the pointer table's write side on 2.5.0"
# Library 2.5.0 stopped publishing a table at commit -- publishing amortises a
# replay and the commit path folds rather than replaying -- which took ~21% off
# a cached bulk load here and left it level with no cache at all.  Two things
# follow that only ZFS can settle:
#
#   the bulk row, because if the table is now free on the write side then the
#   open-side benefit is unopposed and the deployment answer simplifies to
#   "turn it on";
#   the SMALL-transaction row, because a writer no longer moves its own replay
#   window, so rollover_txns governs and a 200k-record load at one record per
#   transaction goes from 12 conversions to 195 (and 4 repacks to 65).  That is
#   far more file lifecycle, and unlink costs 1.8ms here against ~0 on APFS,
#   where it still came out 12% FASTER.  The nosync arm is the sensitive one:
#   the run is short enough for a second of unlinks to dominate it.
for d in "$@"; do
  for cfg in none local; do
    [ "$cfg" = local ] && tbl="zs_index=local" || tbl=""
    for shape in bulk small-durable small-nosync; do
      case $shape in
        bulk)          per=1000; n=$NBIG; extra="" ;;
        small-durable) per=1;    n=$N;    extra="" ;;
        small-nosync)  per=1;    n=$NMID; extra="zs_nosync=1&zs_sync_ms=100000" ;;
      esac
      uri="$tbl"
      [ -n "$extra" ] && uri="${tbl:+$tbl&}$extra"
      w="$d/cached"; rm -rf "$w"; mkdir -p "$w"
      printf -- "-- %s %s table=%s\n" "$d" "$shape" "$cfg"
      if [ -n "$uri" ]; then
        ./zskvbench --dir "$w" -n "$n" --reps 3 --rowid --only $per --uri "$uri"
      else
        ./zskvbench --dir "$w" -n "$n" --reps 3 --rowid --only $per
      fi
      rm -rf "$w"
    done
  done
done > "$OUT/cached.txt" 2>&1
grep -E "^--|per txn|txn each|rewritten" "$OUT/cached.txt" | sed 's/^/  /'
fi

# ---------------------------------------------------------------------- opens
if has_phase opens; then
say "phase: open+first-read latency (the pointer table's other side)"
# The metric a Cyrus-shaped deployment lives on: short-lived processes that
# open, read a little and exit.  A snapshot open replays the active file's
# spans, which is what the pointer table removes -- so this is the benefit
# side of the ~10% the table costs during a load, and the break-even is an
# open:write ratio only this workload can supply.  Built ONE record per
# transaction on purpose: that is what accumulates spans.
for d in "$@"; do
  # Three arms, because the table has two prices.  Enabling it after a cold
  # import costs the import nothing and works as of library 2.4.0 (a read-only
  # handle now creates the cache directory); enabling it during the import
  # costs ~10% of the load and buys about twice as much per open.
  w="$d/opens"; rm -rf "$w"; mkdir -p "$w"
  printf -- "-- %s import cold, read cold\n" "$d"
  ./zskvbench --dir "$w" -n "$N" --reps 3 --rowid --opens 2000
  rm -rf "$w"; mkdir -p "$w"
  printf -- "-- %s import cold, read with table\n" "$d"
  ./zskvbench --dir "$w" -n "$N" --reps 3 --rowid --opens 2000 \
    --uri zs_index=local --build-uri ""
  rm -rf "$w"; mkdir -p "$w"
  printf -- "-- %s import WITH table, read with table\n" "$d"
  ./zskvbench --dir "$w" -n "$N" --reps 3 --rowid --opens 2000 \
    --uri zs_index=local
  rm -rf "$w"
  # The Cyrus configuration: tables on tmpfs rather than beside the database.
  # Indistinguishable from zs_index=local on APFS, where a "disk" read was
  # already RAM -- so this arm only means anything here, where a local table is
  # a raidz2 read through zstd and AES at every open.
  # Linux and BSD spell this differently, and neither fails on the other's
  # syntax -- BSD stat takes -c as the format string and returns "-c", which
  # reads as "not tmpfs" and would skip the arm on the box that can run it.
  # So branch on the OS rather than on an exit code.
  case "$(uname -s)" in
    Linux) idxfs=$(stat -f -c %T "$(dirname "$IDXDIR")" 2>/dev/null) ;;
    *)     idxfs=$(stat -f %T   "$(dirname "$IDXDIR")" 2>/dev/null) ;;
  esac
  if [ "$idxfs" = tmpfs ] || [ "$idxfs" = ramfs ]; then
    w="$d/opens"; rm -rf "$w"; mkdir -p "$w" "$IDXDIR"
    printf -- "-- %s table on tmpfs (%s)\n" "$d" "$IDXDIR"
    ./zskvbench --dir "$w" -n "$N" --reps 3 --rowid --opens 2000 \
      --uri "zs_index_dir=$IDXDIR"
    rm -rf "$w" "$IDXDIR"
  else
    printf -- "-- %s tmpfs arm SKIPPED: %s is %s\n" \
      "$d" "$(dirname "$IDXDIR")" "${idxfs:-unknown}"
  fi
  # And the governing variable, which is how the writer COMMITTED rather than
  # how much it stored: the replay an open pays is the UNSEALED tail of the
  # active file, and the span bound caps that at ~1024 spans.  One row per
  # commit leaves a ~114KB tail; a batching writer lets it reach rollover_size,
  # 2MB, and the table goes from worth ~1.2x to worth 5-15x.  A cliff.
  for per in 1 40 1600; do
    for cfg in none local; do
      w="$d/opens"; rm -rf "$w"; mkdir -p "$w"
      printf -- "-- %s cliff build/txn=%s table=%s\n" "$d" "$per" "$cfg"
      if [ "$cfg" = local ]; then
        ./zskvbench --dir "$w" -n 16000 --reps 3 --rowid --opens 2000 \
          --opens-per "$per" --uri "zs_rollover_txns=1024&zs_index=local" \
          --build-uri "zs_rollover_txns=1024&zs_index=local"
      else
        ./zskvbench --dir "$w" -n 16000 --reps 3 --rowid --opens 2000 \
          --opens-per "$per" --uri "zs_rollover_txns=1024" \
          --build-uri "zs_rollover_txns=1024"
      fi
      rm -rf "$w"
    done
  done
  w="$d/opens"; rm -rf "$w"; mkdir -p "$w"
  printf -- "-- %s stock btree, for scale\n" "$d"
  ./zskvbenchstock --dir "$w" -n "$N" --reps 3 --rowid --opens 2000
  rm -rf "$w"
done > "$OUT/opens.txt" 2>&1
grep -E "^--|open\+first-read" "$OUT/opens.txt" | sed 's/^/  /'
fi

# -------------------------------------------------------------------- latency
if has_phase latency; then
say "phase: commit latency DISTRIBUTION at the per-message shape"
# WHY: every other store row in this tier is a rate, and a rate cannot answer
# the question the deployment actually has.  Cyrus commits one message at a
# time, and the repack cascade runs synchronously inside whichever write
# transaction trips it -- so one delivery in N pays for a whole generation
# merge while the rest pay nothing.  That is a tail problem and a mean hides
# it completely.
#
# The armed/norepack pair is the measurement that decides whether moving
# repacks off the critical path is worth anything: it prices exactly what a
# background repack would remove.  On the laptop it removes the max (2.2ms ->
# 0.8ms) and almost nothing else, because 19 of the 25 slow commits are
# CONVERSIONS, which zs_norepack does not disarm and which cannot be deferred
# -- a generation has to be sealed into sorted form by someone.  Whether ZFS
# agrees is the point of running it here: unlink costs ~1ms on this pool
# against nothing on APFS, and the cascade does a lot of them.
#
# n is deliberately large enough for the cascade to run, and the run is one
# pass (a distribution, not a best-of).
{
for d in "$@"; do
  # armed -> norepack -> norepack+seal, which is the ladder the deployment
  # walks.  norepack takes the REPACKS off the write path and leaves the
  # conversions, which on the laptop are 19 of the 25 slow commits; sealing
  # from idle (2.9.0's zs_db_seal, via sqlite3ZsSeal) preempts those too.  The
  # seal cadence has to beat rollover_txns -- at one record per transaction it
  # is the SPAN bound that seals a generation, not the byte bound, so 1024
  # commits is the number to be under.
  for cfg in "armed::" "norepack:zs_norepack=1:" \
             "norepack+seal/500:zs_norepack=1:500"; do
    lbl=${cfg%%:*}; rest=${cfg#*:}; uri=${rest%%:*}; seal=${rest#*:}
    w="$d/lat"; cleanup "$w"; mkdir -p "$w"
    echo "-- $d cascade $lbl (recordsize $(dataset_recordsize "$d"))"
    ./zskvbench --dir "$w" --latency "$NMID" --rowid \
      ${uri:+--uri "$uri"} ${seal:+--seal-every "$seal"}
    cleanup "$w"
  done
done
} > "$OUT/latency.txt" 2>&1
grep -E "^--|latency|merged|did not merge|rewritten" "$OUT/latency.txt" | sed 's/^/  /'
fi

# ------------------------------------------------------------------- syscalls
if has_phase syscalls; then
say "phase 3/3: syscalls and profile"
# Three ways this can go, decided before anything long runs:
#   unprivileged   strace works as-is: measure the same unprivileged process
#                  the other phases measured, which is what we want.
#   sudo -n        credentials already cached: use them quietly.
#   neither        do NOT flail at a password prompt from inside a script
#                  that has been running for half an hour.  Print the phase
#                  as a paste-ready block and exit cleanly.
probe_strace
[ $STRACE_OK -eq 1 ] || echo "  SKIPPED: the earlier phases are still worth sending."
fi

if has_phase syscalls && [ $STRACE_OK -eq 1 ]; then
d1=${1}
# Two batch sizes, because the syscall MIX depends on it and reading one
# for the other is how a wrong conclusion gets drawn: the per-transaction
# mmap/munmap pair scales with transaction SIZE (a span that still fits the
# 64KB chunk maps nothing), while fdatasync and the lock calls scale with
# commit COUNT.  1-per-txn is also the row where a ZFS-only regression
# turned up unexplained.
for eng in zs stock; do
  case $eng in
    zs)    bin=./zskvbench ;;
    stock) bin=./zskvbenchstock ;;
  esac
  for per in 1000 1; do
    case $per in
      1000) n=$NBIG ;;
      1)    n=$N ;;        # 20000 single commits is already ~40k fdatasyncs
    esac
    w="$d1/sys-$eng-$per"
    cleanup "$w"; mkdir -p "$w"
    echo "-- strace $eng (n=$n, $per per txn)"
    $SUDO strace -c -w -f -o "$OUT/strace-$eng-$per.txt" \
      $bin --dir "$w" -n "$n" --reps 1 --rowid --only $per 2>&1 | tail -2
    cleanup "$w"
  done
done
for f in "$OUT"/strace-zs-*.txt; do
  [ -s "$f" ] || continue
  echo "  top syscalls by time, $(basename "$f"):"
  head -8 "$f" | sed 's/^/    /'
done

if command -v perf >/dev/null 2>&1; then
  # BOTH engines: stock's fdatasync path has never been profiled, and it is the
  # comparison that decides why a synced byte costs us ~9x what it costs the
  # btree.  If stock also lands in zil_commit_impl -> zfs_get_data -> dmu_sync
  # the difference is the data shape; if it does not, it is a ZIL mode
  # difference.
  for eng in zs stock; do
    case $eng in
      zs)    bin=./zskvbench ;;
      stock) bin=./zskvbenchstock ;;
    esac
    w="$d1/sys-perf"; cleanup "$w"; mkdir -p "$w"
    echo "-- perf $eng $NBIG"
    if $SUDO perf record -g -o "$OUT/perf-$eng.data" -- \
         $bin --dir "$w" -n "$NBIG" --reps 1 --rowid --only 1000 \
         > "$OUT/perf-run-$eng.txt" 2>&1; then
      $SUDO perf report -i "$OUT/perf-$eng.data" --stdio --sort=dso,sym \
        2>/dev/null | head -40 > "$OUT/perf-$eng.txt"
      sed 's/^/    /' "$OUT/perf-$eng.txt" | head -8
    else
      echo "  perf record failed (perf_event_paranoid?), see $OUT/perf-run-$eng.txt"
    fi
    cleanup "$w"
  done
else
  echo "  perf not installed, skipping (the strace counts are the important half)"
fi
fi

# ---------------------------------------------------------------------- fsync
if has_phase fsync; then
say "phase: per-fsync cost against per-commit append size"
# WHY: at 1000 records per transaction our fdatasync costs 302us and stock's
# 23us; at ONE record ours costs 11.8us and stock's 15.4us.  So the cost is not
# ours inherently, it scales with the size of the append being synced -- and
# perf puts 19.84% of all cycles in zil_lwb_write_issue -> zfs_get_data ->
# dmu_sync, which is the ZIL path taken when a log entry does not carry its
# payload inline: write the real blocks to their final location, with parity,
# compression and encryption, before fdatasync returns.
#
# RESULT (2026-08-18): the threshold story was WRONG.  Ours is a smooth floor
# plus a per-KB term with no knee at 32KB or anywhere, and stock is flat as
# predicted -- so the size-scaling is real and the mechanism was invented.
#
# RESULT (2026-08-19): every number this phase ever printed was SYSTEM time,
# because `strace -c` summarises system time unless you pass -w.  Both call
# sites now pass it.  A blocking fdatasync sleeps, and sleeping is not system
# time, so this phase reported 12us for a gate whose real latency is 86us and
# I built a prediction on it.  Cross-check any number here against the paired
# durable/nosync matrix rows, which measure wall time by construction.
#
# RESULT (2026-08-19, later): in wall time there is no per-byte DEFICIT at all
# -- we are cheaper per fdatasync than stock at every transaction size, because
# the ~125us ZIL floor is shared and stock pays it 4x per commit against our 1x.
# The CPU asymmetry that remains is NOT crypto: a call graph puts our
# fdatasync's CPU in zio_create (alloc+memset), taskq dispatch and wakeup,
# zfs_zget and the rangelock, with zio_compress_select at 0.03% and zstd/aes
# absent.  ZIO_STAGE_ISSUE_ASYNC precedes ZIO_STAGE_WRITE_COMPRESS, so both
# engines' crypto is on a z_wr_iss taskq.
#
# So the live question is per-BLOCK cost, not per-byte: a 130KB span is ~32
# blocks at recordsize=4K and one or two at 128K, which should mostly flatten
# the slope.  This phase therefore sweeps EVERY dataset it was given, not just
# the first -- it ran on $1 alone for three rounds and could not have seen it.
probe_strace
if [ $STRACE_OK -ne 1 ]; then
  echo "  SKIPPED: needs strace"
else
echo "  zfs_immediate_write_sz: $(cat /sys/module/zfs/parameters/zfs_immediate_write_sz 2>/dev/null || echo '(unreadable)')"
for d1 in "$@"; do
  echo "-- $d1 (recordsize $(dataset_recordsize "$d1"))"
  for per in 1 10 100 200 400 1000; do
    case $per in
      1) n=$N ;;
      *) n=$NMID ;;
    esac
    for eng in zs stock; do
      case $eng in
        zs)    bin=./zskvbench ;;
        stock) bin=./zskvbenchstock ;;
      esac
      w="$d1/fs-$eng-$per"
      cleanup "$w"; mkdir -p "$w"
      f="$OUT/fsync-$(basename "$d1")-$eng-$per.txt"
      $SUDO strace -c -w -f -o "$f" \
        $bin --dir "$w" -n "$n" --reps 1 --rowid --only $per >/dev/null 2>&1
      line=$(grep -E "[[:space:]]fdatasync$" "$f" 2>/dev/null | head -1)
      printf "  %-5s per=%-5s append=%-8s %s\n" "$eng" "$per" \
        "$((per*111))B" "$(echo "$line" | awk '{printf "%s calls, %s us/call", $4, $3}')"
      cleanup "$w"
    done
  done
done
fi
fi

say "done"
echo "send these:"
ls -1 "$OUT" | sed 's/^/  /'
echo
# Every file the phases produce, derived from the phase list rather than
# hand-maintained: a stale hint here silently loses the newest phase's output,
# which is exactly what happened to the cached and fsync phases.
echo "  grep . $OUT/*.txt 2>/dev/null"
