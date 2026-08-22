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

# 4b. a database in on-disk FORMAT 2, written by library 2.9.1 (the
#    version vendored immediately before format 3).  Distinct from case 4:
#    that fixture is an older FILE NAMING layout, this one is the current
#    naming with the previous FORMAT -- the magic's major version digit is
#    '1' where a 3.0.0 writer puts '2'.  Regenerable, unlike case 4's:
#    build zskvbench against the 2.9.1 sources and run --build-only.
#
#    The fixture holds a real 50-row `kv` table and its schema is still
#    legible in the file (`strings` finds the CREATE TABLE), so a count of
#    0 here is the engine declining to read data that is demonstrably
#    present -- not an empty fixture passing vacuously.  Upstream rejects a
#    foreign format at the magic (F-6b, R-6) and implements no migrator,
#    which at this layer surfaces as an EMPTY database rather than an
#    error.  Nothing has ever shipped on format 2 so nothing needs
#    carrying forward, but a leftover benchmark or test directory from
#    before 2026-08-21 will read empty and a write will start a fresh
#    generation beside data still on disk.  If a version mismatch ever
#    becomes a hard error, this expectation flips.
if [ -d "$(dirname "$0")/fixtures/fmt2-db" ]; then
  cp -r "$(dirname "$0")/fixtures/fmt2-db" "$W/dbfmt2"
  chmod -R u+w "$W/dbfmt2"
  out=$($SH "$W/dbfmt2" "SELECT count(*) FROM sqlite_master;" 2>&1)
  expect "format-2 database opens as empty (see header)" "0" "$out"
  # The bytes are still there: this asserts the fixture is not merely an
  # empty directory, which would make the case above pass for free.
  n=$(strings "$W"/dbfmt2/*.current 2>/dev/null | grep -c "CREATE TABLE kv")
  expect "...with its schema still on disk, unread" "1" "$n"
fi

# 5. a database created by THIS build reopens cleanly (the trivial case
#    that a format change must not break)
$SH "$W/db4" "CREATE TABLE t(a INTEGER PRIMARY KEY, b); INSERT INTO t VALUES(1,'x');" >/dev/null 2>&1
out=$($SH "$W/db4" "SELECT b FROM t WHERE a=1;" 2>&1)
expect "round-trip through a reopen" "x" "$out"

[ $fails = 0 ] && echo "format-guard ok" || echo "format-guard: $fails failure(s)"
exit $fails
