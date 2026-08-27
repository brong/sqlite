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

# 4b/4c. databases in the two SUPERSEDED on-disk formats, in the current
#    file naming.  Distinct from case 4, which is an older file NAMING with
#    a format this build could still read.  The magic's version digit tells
#    them apart: '1' is format 2, '2' is format 3, '4' is format 4.  (The
#    digit began as a count of incompatible formats and became the version
#    at 4, so '3' is never written and format 3 was withdrawn entirely --
#    do not infer one number from the other.)
#
#    Both fixtures hold a real 50-row `kv` table whose schema is still
#    legible in the file, so a count of 0 is the engine declining to read
#    data that is demonstrably present -- not an empty fixture passing
#    vacuously.  Regenerate fmt2-db by building zskvbench against the 2.9.1
#    sources and running --build-only; fmt3-db was written by this tree at
#    commit f8027d7a7e with the SQL in the case below.
#
#    UPSTREAM PREDICTED THESE WOULD FLIP AT VERSION 4 AND THEY DID NOT.
#    4.0.0 refuses a version-2 or version-3 file at open rather than
#    part-parsing it -- but that refusal is per FILE, and zs_db_open still
#    skips a member it cannot decode rather than failing the open.  With
#    every member refused the directory is indistinguishable from an empty
#    one to a caller opening with ZS_CREATE, which is what this engine
#    does, so the caller-visible answer is still an EMPTY DATABASE and not
#    an error.  Case 4d below is where the refusal actually bites.
for pair in "fmt2-db:format-2" "fmt3-db:format-3"; do
  fx=${pair%%:*}; label=${pair#*:}
  [ -d "$(dirname "$0")/fixtures/$fx" ] || continue
  rm -rf "$W/db-$fx"
  cp -r "$(dirname "$0")/fixtures/$fx" "$W/db-$fx"
  chmod -R u+w "$W/db-$fx"
  out=$($SH "$W/db-$fx" "SELECT count(*) FROM sqlite_master;" 2>&1)
  expect "$label database opens as empty (see header)" "0" "$out"
  # The bytes are still there: this asserts the fixture is not merely an
  # empty directory, which would make the case above pass for free.
  n=$(strings "$W"/db-$fx/*.current 2>/dev/null | grep -c "CREATE TABLE kv")
  expect "...with its schema still on disk, unread" "1" "$n"
  # A superseded member ALONGSIDE a readable database is a different
  # question, and there the refusal does surface: the open no longer has
  # the "nothing here, create" escape.  This is the discriminator that
  # proves the case above is about the DIRECTORY being empty of readable
  # members, not about the old format being tolerated.
  rm -rf "$W/mix-$fx"
  $SH "$W/mix-$fx" "CREATE TABLE t(a); INSERT INTO t VALUES(42);" >/dev/null 2>&1
  cp "$(dirname "$0")/fixtures/$fx"/*.current "$W/mix-$fx/"
  chmod -R u+w "$W/mix-$fx"
  out=$($SH "$W/mix-$fx" "SELECT * FROM t;" 2>&1)
  expect "...but beside a readable database it IS an error" "malformed" "$out"
done

# 4d. THE HAZARD, and the reason "dump and reload" is an instruction to
#    follow BEFORE upgrading rather than after noticing.  A superseded
#    database reads as empty (4b/4c) and the bytes survive that read -- but
#    the first WRITE reuses the same active-file name and TRUNCATES it.  The
#    old data is not left beside a fresh generation; it is gone.
#
#    This is not new at version 4: the format-3 build destroys a format-2
#    directory the same way (measured 2026-08-27, both arms, 12712 -> ~280
#    bytes with the same file name).  It went unasserted because the header
#    of this file, and the doc, both described it as "a fresh generation
#    beside data still on disk", which is what a NEW uuid would have given.
#    Asserting it means a future library that starts refusing the whole
#    open -- the safe behaviour -- fails here and gets noticed.
if [ -d "$(dirname "$0")/fixtures/fmt2-db" ]; then
  rm -rf "$W/dbdestroy"
  cp -r "$(dirname "$0")/fixtures/fmt2-db" "$W/dbdestroy"
  chmod -R u+w "$W/dbdestroy"
  before=$(wc -c < "$W"/dbdestroy/*.current | tr -d ' ')
  # a read alone is safe: it only adds the lock file
  $SH "$W/dbdestroy" "SELECT count(*) FROM sqlite_master;" >/dev/null 2>&1
  n=$(strings "$W"/dbdestroy/*.current 2>/dev/null | grep -c "CREATE TABLE kv")
  expect "a READ leaves the superseded bytes intact" "1" "$n"
  $SH "$W/dbdestroy" "CREATE TABLE n(x); INSERT INTO n VALUES(7);" >/dev/null 2>&1
  after=$(wc -c < "$W"/dbdestroy/*.current | tr -d ' ')
  n=$(strings "$W"/dbdestroy/*.current 2>/dev/null | grep -c "CREATE TABLE kv")
  expect "a WRITE destroys them (same file name, truncated)" "0" "$n"
  echo "     (active file $before -> $after bytes)"
fi

# 5. a database created by THIS build reopens cleanly (the trivial case
#    that a format change must not break)
$SH "$W/db4" "CREATE TABLE t(a INTEGER PRIMARY KEY, b); INSERT INTO t VALUES(1,'x');" >/dev/null 2>&1
out=$($SH "$W/db4" "SELECT b FROM t WHERE a=1;" 2>&1)
expect "round-trip through a reopen" "x" "$out"

[ $fails = 0 ] && echo "format-guard ok" || echo "format-guard: $fails failure(s)"
exit $fails
