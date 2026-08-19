#!/bin/bash
# Run the zeroskip permutation's file list, one testfixturezs process
# per file in a per-file scratch directory, so that neither a hard
# failure nor leftover state (a zeroskip database is a DIRECTORY, which
# several tests' single-file cleanup idioms cannot remove) can affect
# another file.  Usage: run-suite.sh [builddir] [listfile] ; writes
# per-file results to /tmp/zs-suite-results.txt and prints a summary.
set -u
BUILD="${1:-.}"
RESULTS=/tmp/zs-suite-results.txt
LIST="${2:-/tmp/zs-suite-files.txt}"
cd "$BUILD"
BUILD=$(pwd)
if [ -z "${2:-}" ]; then
  ZS_LIST_ONLY=1 ./testfixturezs test/permutations.test zeroskip 2>/dev/null \
    | grep "^FILE " | sed 's/^FILE //' > "$LIST"
fi
n=$(wc -l < "$LIST" | tr -d ' ')
if [ "$n" -eq 0 ]; then
  # An empty list is not "everything passed".  The usual cause on macOS
  # is a testfixturezs produced by cp'ing OVER an existing binary: the
  # kernel keeps the old code signature, SIGKILLs the process at exec,
  # and you get no output and no exit status worth reading.  Delete the
  # target before copying: rm -f testfixturezs && cp testfixture testfixturezs
  echo "ERROR: no test files listed -- is ./testfixturezs runnable?" >&2
  ./testfixturezs /dev/null >/dev/null 2>&1 \
    || echo "       ./testfixturezs does not execute (exit $?)" >&2
  exit 1
fi
echo "running $n files..."
: > "$RESULTS"
WORK=${TMPDIR:-/tmp}/zs-suite-work
rm -rf "$WORK"; mkdir -p "$WORK"
i=0
while IFS= read -r f; do
  i=$((i+1))
  base=$(basename "$f")
  case "$f" in
    /*)  path="$f" ;;                   # the list carries ext/ paths too
    */*) path="$BUILD/$f" ;;
    *)   path="$BUILD/test/$f" ;;       # bare names live in test/
  esac
  if [ ! -f "$path" ]; then
    echo "$base MISSING-PATH" >> "$RESULTS"; continue
  fi
  rm -rf "$WORK/run"; mkdir -p "$WORK/run"
  cp "$BUILD/testfixturezs" "$WORK/run/"
  out=$(cd "$WORK/run" && perl -e 'alarm 600; exec @ARGV' \
        ./testfixturezs "$path" 2>&1)
  rc=$?
  [ $rc -eq 142 ] && rc=124   # SIGALRM
  line=$(echo "$out" | grep -E "errors out of" | tail -1)
  if [ $rc -eq 124 ]; then
    echo "$base TIMEOUT" >> "$RESULTS"
  elif [ -z "$line" ] && [ $rc -eq 0 ]; then
    # exited cleanly with no summary: the file gated itself out (platform
    # or capability) and ran no tests.  Stock does the same for these.
    echo "$base 0 (no tests run)" >> "$RESULTS"
  elif [ -z "$line" ]; then
    echo "$base CRASHED rc=$rc" >> "$RESULTS"
  else
    errs=$(echo "$line" | awk '{print $1}')
    fails=$(echo "$out" | grep "Failures on these" | tail -1 | cut -c26-160)
    echo "$base $errs $fails" >> "$RESULTS"
  fi
  if [ $((i % 50)) -eq 0 ]; then echo "  $i/$n done"; fi
done < "$LIST"
rm -rf "$WORK"
echo "--- summary"
awk '$2!=0 {print}' "$RESULTS" | head -80
echo "clean files: $(awk '$2==0' "$RESULTS" | wc -l | tr -d ' ') / $n"
