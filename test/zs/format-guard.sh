#!/bin/bash
# What does the engine do with a database it cannot parse?
#
# This matters across a zeroskip format change: if a new library cannot
# read an old database, the difference between "hard error" and "opens
# as empty" is the difference between a clear failure and silently
# writing a fresh generation alongside data that is still on disk.
#
# The expectations below record BEHAVIOUR AS OBSERVED, not behaviour as
# desired -- case 2 in particular is the shape worth arguing about
# upstream.  When a re-vendor changes an answer, this fails and the
# change gets a decision instead of going unnoticed.
#
# Usage: format-guard.sh [builddir]
set -u
BUILD="${1:-.}"
cd "$BUILD"
SH=./sqlite3zs
W="${TMPDIR:-/tmp}/zs-format-guard.$$"
rm -rf "$W"; mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
fails=0

expect() { # expect <label> <pattern> <actual>
  case "$3" in
    *$2*) echo "ok   $1" ;;
    *)    echo "FAIL $1: expected to match '$2', got: $3"; fails=$((fails+1)) ;;
  esac
}

# 1. an unparseable file that IS a database member (a .current name) is
#    detected; one that is not a member (the pre-2026-08-14 generation
#    naming, or anything else) is ignored.
$SH "$W/db1" "CREATE TABLE t(a); INSERT INTO t VALUES(42);" >/dev/null 2>&1
printf 'garbage from another format' \
  > "$W/db1/zeroskip-ffffffff-ffff-ffff-ffff-ffffffffffff.current"
out=$($SH "$W/db1" "SELECT * FROM t;" 2>&1)
expect "unparseable member (.current) is rejected" "malformed" "$out"
rm -f "$W/db1/zeroskip-ffffffff-ffff-ffff-ffff-ffffffffffff.current"

printf 'junk' \
  > "$W/db1/zeroskip-ffffffff-ffff-ffff-ffff-ffffffffffff-00000009"
out=$($SH "$W/db1" "SELECT * FROM t;" 2>&1)
expect "non-member file is ignored" "42" "$out"

# 2. a directory containing only unparseable files opens as EMPTY
mkdir -p "$W/db2"
printf 'not a zeroskip file' > "$W/db2/zeroskip-deadbeef-00000001"
out=$($SH "$W/db2" "SELECT count(*) FROM sqlite_master;" 2>&1)
expect "unparseable-only directory opens as empty (see header)" "0" "$out"

# 3. a real database whose only file has a destroyed header reads empty
$SH "$W/db3" "CREATE TABLE t(a); INSERT INTO t VALUES(1);" >/dev/null 2>&1
f=$(ls "$W"/db3/zeroskip-* 2>/dev/null | head -1)
printf 'XXXXXXXXXXXXXXXX' | dd of="$f" bs=1 seek=0 conv=notrunc 2>/dev/null
out=$($SH "$W/db3" "SELECT * FROM t;" 2>&1)
expect "destroyed header: table is gone, not an error" "no such table" "$out"

# 4. a database in the PRE-2026-08-14 on-disk layout (active file named
#    zeroskip-<uuid>-<GEN> rather than zeroskip-<uuid>.current).  The
#    fixture in fixtures/oldfmt-db cannot be regenerated -- no build in
#    this tree can still write that layout -- so it is checked in.
#
#    Upstream deliberately implements no fallback (F-7a), and the
#    result is that such a database opens as EMPTY: no error, and a
#    subsequent write starts a fresh generation beside data that is
#    still on disk and now invisible.  Even a read mutates the
#    directory, because the engine opens with ZS_CREATE.  If a version
#    mismatch ever becomes a hard error, this expectation flips.
if [ -d "$(dirname "$0")/fixtures/oldfmt-db" ]; then
  cp -r "$(dirname "$0")/fixtures/oldfmt-db" "$W/dbold"
  chmod -R u+w "$W/dbold"
  out=$($SH "$W/dbold" "SELECT count(*) FROM sqlite_master;" 2>&1)
  expect "pre-format-change database opens as empty (see header)" "0" "$out"
fi

# 5. a database created by THIS build reopens cleanly (the trivial case
#    that a format change must not break)
$SH "$W/db4" "CREATE TABLE t(a INTEGER PRIMARY KEY, b); INSERT INTO t VALUES(1,'x');" >/dev/null 2>&1
out=$($SH "$W/db4" "SELECT b FROM t WHERE a=1;" 2>&1)
expect "round-trip through a reopen" "x" "$out"

[ $fails = 0 ] && echo "format-guard ok" || echo "format-guard: $fails failure(s)"
exit $fails
