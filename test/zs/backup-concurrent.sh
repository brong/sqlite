#!/bin/bash
# Backup taken while a writer hammers the source must be a consistent
# snapshot: integrity_check passes and content is self-consistent.
set -u
BUILD="${1:-.}"
DBDIR="$(mktemp -d "${TMPDIR:-/tmp}/zsbk-XXXXXX")"
DB="$DBDIR/src"
DEST="$DBDIR/dest"
"$BUILD/sqlite3zs" "$DB" 'CREATE TABLE t(a INTEGER PRIMARY KEY, b);
INSERT INTO t VALUES(1,1),(2,2);' || exit 1
(
  for i in $(seq 1 500); do
    "$BUILD/sqlite3zs" "$DB" "INSERT INTO t(b) VALUES(hex(randomblob(8)));" 2>/dev/null
  done
) &
W=$!
sleep 0.3
"$BUILD/sqlite3zs" "$DB" ".timeout 5000" ".backup main $DEST" || { kill $W; exit 1; }
kill $W 2>/dev/null; wait $W 2>/dev/null
ic="$("$BUILD/sqlite3zs" "$DEST" 'PRAGMA integrity_check; SELECT count(*)>=2 FROM t;')"
echo "$ic" | grep -qx ok || { echo "FAIL: integrity: $ic"; rm -rf "$DBDIR"; exit 1; }
echo "$ic" | grep -qx 1 || { echo "FAIL: count: $ic"; rm -rf "$DBDIR"; exit 1; }
rm -rf "$DBDIR"
echo "backup-concurrent ok"
