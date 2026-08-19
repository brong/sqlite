#!/bin/bash
# Writer inserts in one big transaction; kill -9 it mid-flight; reopen
# and verify the database is intact: either the whole insert committed
# or none of it.
set -u
BUILD="${1:-.}"
fails=0
for round in 1 2 3 4 5 6 7 8 9 10; do
  DBDIR="$(mktemp -d "${TMPDIR:-/tmp}/zscrash-XXXXXX")"
  DB="$DBDIR/db"
  "$BUILD/sqlite3zs" "$DB" 'CREATE TABLE t(a INTEGER PRIMARY KEY, b);' || exit 1
  "$BUILD/sqlite3zs" "$DB" 'WITH RECURSIVE s(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM s WHERE x<100000)
    INSERT INTO t SELECT x, hex(randomblob(64)) FROM s;' &
  W=$!
  # vary the kill timing across rounds
  sleep 0.0$round
  kill -9 $W 2>/dev/null
  wait $W 2>/dev/null
  out="$("$BUILD/sqlite3zs" "$DB" 'PRAGMA integrity_check; SELECT count(*) IN (0,100000) FROM t;')"
  if [ "$out" != "ok
1" ]; then
    echo "FAIL round $round: $out"
    fails=1
  fi
  rm -rf "$DBDIR"
done
[ $fails = 0 ] && echo "crash-test ok (10 rounds)"
exit $fails
