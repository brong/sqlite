#!/bin/bash
# Two processes: a writer holds a transaction open; a reader must not
# block; a second writer with a timeout succeeds after the first
# commits; one with no timeout reports busy.
set -u
BUILD="${1:-.}"
DBDIR="$(mktemp -d "${TMPDIR:-/tmp}/zsbusy-XXXXXX")"
DB="$DBDIR/db"
fail=0
"$BUILD/sqlite3zs" "$DB" 'CREATE TABLE t(a INTEGER PRIMARY KEY, b); INSERT INTO t VALUES(1,1);' || exit 1
( printf 'BEGIN IMMEDIATE;\nINSERT INTO t VALUES(2,2);\n'; sleep 2; printf 'COMMIT;\n' ) \
  | "$BUILD/sqlite3zs" -batch "$DB" &
W=$!
sleep 0.5
# lock-free reader while the writer transaction is open
r="$("$BUILD/sqlite3zs" "$DB" 'SELECT count(*) FROM t;')"
[ "$r" = "1" ] || { echo "FAIL: reader saw '$r' (want 1: snapshot before commit)"; fail=1; }
# writer without timeout: busy
b="$("$BUILD/sqlite3zs" "$DB" 'INSERT INTO t VALUES(10,10);' 2>&1)"
echo "$b" | grep -q "locked\|busy" || { echo "FAIL: expected busy, got '$b'"; fail=1; }
# writer with timeout: succeeds once the first commits
"$BUILD/sqlite3zs" "$DB" '.timeout 5000' 'INSERT INTO t VALUES(11,11);' || { echo "FAIL: timed writer"; fail=1; }
wait $W
n="$("$BUILD/sqlite3zs" "$DB" 'SELECT count(*) FROM t;')"
[ "$n" = "3" ] || { echo "FAIL: final count $n (want 3)"; fail=1; }
rm -rf "$DBDIR"
[ $fail = 0 ] && echo "busy-test ok"
exit $fail
