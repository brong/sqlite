#!/bin/sh
# Does library 2.9.0's posix_madvise(WILLNEED) on merge inputs do anything?
#
# Upstream added it from our call graph, which put 11.6% of a bulk load in page
# faults under XXH3_hashLong -- the verify pass that opens a conversion or a
# repack, servicing one synchronous fault per page instead of hashing.  They
# cannot measure it (APFS: the pages are already resident) and neither can this
# laptop.  Their position is that if it does not show on ZFS it has no
# justification and should come out, so this script is the measurement that
# decides it.
#
# WHY IT NEEDS ITS OWN SCRIPT, and why prodrun's cascade phase cannot answer it:
# every fixture in this tree writes its merge input and merges it moments later.
# A hint can only help when the pages are NOT resident, so the existing phases
# are the worst possible case for observing one, and an A/B over them would
# measure nothing and wrongly condemn the change.  The phases therefore run in
# SEPARATE PROCESSES with a cache drop between them:
#
#   zskvbench --build-only     leave a database, cascade disarmed, files unmerged
#   echo 3 > drop_caches       page cache cold
#   zskvbench --catchup-only   time the merge alone, and count its faults
#
# Faults are the primary read, not the clock.  The hint acts on faults, a fault
# count is nearly noise-free where a wall clock on a shared machine is not, and
# `getrusage` gives both for free.
#
# HONEST LIMIT, state it with any result: drop_caches empties the PAGE cache and
# not the ARC, so a fault afterwards still finds its data in ARC rather than on
# disk.  That is cheaper than a real cold read, so this UNDERSTATES the hint.
# Making it colder means exporting and re-importing the pool, or a database
# larger than ARC -- neither of which belongs in a script pointed at a
# production box.  If the effect shows here it is real; if it does not, that is
# evidence but not proof, and the next step is a dataset bigger than ARC.
set -eu

DIR=${1:?usage: prefetch-ab.sh /mnt/dataset [nrecs] [passes]}
N=${2:-2000000}
PASSES=${3:-3}
OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE'
TOP=$(cd "$(dirname "$0")/../.." && pwd)
cd "$TOP"

SUDO=
if [ "$(id -u)" != 0 ]; then
  if sudo -n true 2>/dev/null; then SUDO="sudo -n"; else
    echo "ERROR: dropping the page cache needs root, and sudo cannot prompt." >&2
    echo "       run:  sudo -v && $0 $*" >&2
    exit 1
  fi
fi

# The two arms.  The current tree is the WITH-hint arm; the without is the most
# recent vendoring whose zeroskip.c CONTAINS NO posix_madvise.
#
# Selected by CONTENT, not by history position, because the first version of
# this script used "the previous commit that touched zeroskip.c" and that was
# wrong the moment an unrelated vendoring landed on top: at 2.9.1 the previous
# touch was the commit that INTRODUCED the hint, so both arms had it and the
# run measured nothing -- identical major-fault counts to the unit across six
# runs, which read as a clean null result and is really a null instrument.
#
# The cmp guard did not catch it either, because the lock-protocol change made
# the binaries differ in a dimension that had nothing to do with the hint.  So
# the assertion below is on the posix_madvise COUNT (2 against 0), which is the
# thing under test, and cmp is kept only as a build sanity check.
say(){ printf '\n== %s\n' "$*"; }
build_arm(){                       # build_arm <label> <treeish-or-CURRENT>
  rm -f zeroskip.o btree_zs.o libsqlite3.a zskvbench
  if [ "$2" != CURRENT ]; then
    git show "$2:ext/zeroskip/zeroskip.c" > ext/zeroskip/zeroskip.c
  else
    git checkout -- ext/zeroskip/zeroskip.c
  fi
  make USE_AMALGAMATION=0 OPTIONS="$OPTIONS" lib zskvbench >/dev/null 2>&1
  cp zskvbench "zskvbench-$1"
  git checkout -- ext/zeroskip/zeroskip.c
}
say "selecting the arms by content"
NHINT=$(grep -c posix_madvise ext/zeroskip/zeroskip.c || true)
if [ "$NHINT" -eq 0 ]; then
  echo "ERROR: the current tree has no posix_madvise -- nothing to test." >&2
  exit 1
fi
PREV=
for c in $(git rev-list HEAD -- ext/zeroskip/zeroskip.c); do
  if [ "$(git show "${c}:ext/zeroskip/zeroskip.c" | grep -c posix_madvise)" -eq 0 ]; then
    PREV=$c; break
  fi
done
if [ -z "$PREV" ]; then
  echo "ERROR: no vendoring of zeroskip.c without posix_madvise -- cannot" >&2
  echo "       form a without-hint arm.  Do NOT fall back to 'the previous" >&2
  echo "       commit': that is how this script once compared a build to" >&2
  echo "       itself and reported a clean null." >&2
  exit 1
fi
echo "  hint   = current tree ($(sed -n 's/^source: //p' ext/zeroskip/VENDOR)), $NHINT call sites"
echo "  nohint = $PREV, 0 call sites"
say "building both arms"
build_arm hint CURRENT
build_arm nohint "$PREV"
if cmp -s zskvbench-hint zskvbench-nohint; then
  echo "ERROR: the two arms are byte-identical -- nothing was measured." >&2
  exit 1
fi

W="$DIR/prefetch-ab"
run_arm(){                         # run_arm <label>
  rm -rf "$W"; mkdir -p "$W"
  "./zskvbench-$1" --dir "$W" -n "$N" --rowid --build-only >/dev/null
  sync
  $SUDO sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
  printf '  %-7s ' "$1"
  "./zskvbench-$1" --dir "$W" -n "$N" --rowid --catchup-only \
    | awk '/cold catch-up/{t=$3" "$4" "$5} /page faults/{f=$3" "$4" "$5" "$6}
           END{printf "%-24s %s\n", t, f}'
  rm -rf "$W"
}
say "$N records, $PASSES passes, arm order alternating"
p=1
while [ "$p" -le "$PASSES" ]; do
  echo "-- pass $p"
  # Alternate the order.  A fixed arm order manufactured a clean-looking 2.4%
  # with non-overlapping ranges once already this project; the fix is to make
  # the order part of the design rather than an accident.
  if [ $((p % 2)) -eq 1 ]; then
    run_arm nohint; run_arm hint
  else
    run_arm hint; run_arm nohint
  fi
  p=$((p + 1))
done
say "done -- read the FAULT columns first, the clock second"
