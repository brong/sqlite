#!/bin/bash
# Runs each test/zs/NN-*.sql against a fresh zeroskip db with sqlite3zs
# from the build directory, diffing output against NN-*.expected.
# Usage: run-tests.sh [builddir]
set -u
BUILD="${1:-.}"
SHELL_BIN="$BUILD/sqlite3zs"
SRC="$(cd "$(dirname "$0")" && pwd)"
fail=0
rm -rf /tmp/zs-backup-dest
for t in "$SRC"/[0-9][0-9]-*.sql; do
  base="${t%.sql}"
  name="$(basename "$base")"
  dbdir="$(mktemp -d "${TMPDIR:-/tmp}/zstest-XXXXXX")"
  db="$dbdir/db"
  got="$("$SHELL_BIN" -batch "$db" < "$t" 2>&1)"
  want="$(cat "$base.expected")"
  if [ "$got" = "$want" ]; then
    echo "ok   $name"
  else
    echo "FAIL $name"
    diff <(echo "$want") <(echo "$got") | head -20
    fail=1
  fi
  rm -rf "$dbdir" /tmp/zs-backup-dest
done
exit $fail
