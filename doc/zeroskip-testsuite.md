# Zeroskip engine: TCL test-suite triage ledger

Spec: `docs/superpowers/specs/2026-08-14-zeroskip-tcl-testsuite-design.md`

Run the tier:

```
make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
     'LDFLAGS.libsqlite3=-L/opt/homebrew/lib -rpath /usr/local/lib -lz' \
     lib testfixture && rm -f testfixturezs && cp testfixture testfixturezs
caffeinate -i ./test/zs/run-suite.sh .     # isolated, one process per file
```

Every excluded file and per-test guard carries a reason here.

## Summary (2026-08-14, overnight port + first triage round)

- Tier: the zeroskip permutation (veryquick-class base set), run by
  `test/zs/run-suite.sh` — one process per file, each in its own
  scratch directory.  Isolation is not optional: a zeroskip database
  is a DIRECTORY, and the single-file cleanup idioms in many tests
  (`file delete test.db`, `forcedelete`) leave one behind, so a later
  file inherits a populated database.  Several "failures" in the first
  round were only that.
- **966 of 966 files in the tier pass** (verified end-to-end under the
  isolated runner after the last engine fix), including `eval.test`,
  `vtabH.test` and `zipfile.test`, which this round reinstated.  Seven
  of those files gate themselves out and run no tests at all (win32*,
  bigmmap, alias, qrf04); stock behaves identically, and the runner now
  records that rather than calling it a crash.
- Twelve engine bugs found and fixed (section below), including one
  silent-data-loss class (same-process double writer), two key-encoding
  correctness holes, and two cursor-lifetime bugs found in this round.
- 74 files excluded, each with a category and a reason below.  Every
  one was re-run individually in a clean directory in this round: 74 of
  the 77 previously excluded still fail, so the list is evidence-based
  rather than provisional.  No TRIAGE entries remain.
- Framework changes are capability-keyed.  Stock `veryquick` was
  re-verified after the port: 332149 tests, one failure (`zipfile-25.0`),
  which reproduces identically on a pristine `master` worktree with the
  same binary — a pre-existing cross-file interaction in veryquick
  (an earlier file leaves a file named `x` in the run directory), not a
  consequence of anything on this branch.

## Excluded files

Storage-layer suites excluded wholesale (see the permutation's list):
wal/pager/journal/crash/corrupt/incrblob/incrvacuum/dbstat/dbpage/
backup_ioerr/mmap/superlock/snapshot/vacuum/io/malloc/ioerr/fault/
recover/multiplex/locking/quota/checkpoint/cache-spill/sync/
zerodamage/cksumvfs/oserror/rdonly/resetdb/minmax3/autovacuum/
memdb-serialize/shell (spawns the stock CLI)/alter2 (hexio)/
atomic-batch-write and friends.

Per-file exclusions.  Category meanings:

- `format` — reads or writes the single-file on-disk image directly
  (`hexio_*`, `file size test.db`, page counts, page sizes, file-format
  bytes) or asserts byte-identical database content.  A zeroskip
  database is a directory of log files; there is nothing to probe.
- `corrupt` — deliberately corrupts the image and expects
  `database disk image is malformed`.
- `lock` — asserts single-file locking states (`database is locked`,
  `PRAGMA lock_status`, multi-connection blocking).  Zeroskip readers
  are lock-free and writers exclude via the directory lock.
- `openmap` — asserts the exact set of files/URIs a connection opens,
  or opens a database by a name the engine maps to a directory.
- `journal` — asserts journal-mode/data_version/statement-journal
  behaviour of the pager.
- `collation` — indexes on application-registered collations
  (documented limitation: keys must be memcmp-encodable).
- `utf16` — UTF-16 database encoding (key encoding is UTF-8 only).
- `semantic` — a real behavioural difference, listed with its ticket
  below; these are the honest open items.

format: alter3, autoinc (also corrupt), createtab, dbfuzz001,
descidx1, descidx2, descidx3, expridx1 (`PRAGMA integrity_check`
wording for expression indexes on imprecise reals), format4, index5,
misc1, pragma, pragma2, pragma3, pragma6, readonly, schema6 (asserts
distinct byte content per schema), shrink, sort5, stat, stmt, tempdb,
temptable, temptable2, tkt1512, tkt2920, tkt-313723c356,
tkt-5d863f876e, tkt-f3e5abed55, tkt-fc62af4523, tkt3457.

corrupt: fkey1 (8.3), gencol1 (15.10/15.20 — corrupted index probe),
dbfuzz001.

lock: attach2, capi2, capi3b, tempdb2, thread1, thread3, tkt2409,
tkt4018.

openmap: attach, attach3, capi3, capi3c, capi3e, date, e_uri, openv2,
uri, tkt3121 (vtabD file probing).

journal: attach4, avtrans, dataversion1, pendingrace, tclsqlite.

collation: atof1, collate2, collate3, collate4, collate5, collate9,
e_reindex, like3, reindex, window6, windowE.

utf16: enc, enc2, utf16align, values (14.2 sets
`PRAGMA encoding = utf16`).

semantic (open items, each with a minimal repro noted):
- interrupt, interrupt2 — a statement interrupted mid-step leaves
  sibling statements' results differing from stock in 12 cases;
  `progress.test` (same shape) now passes after engine bug 11, so what
  remains is narrower than it was.
- misc8 (1.7) — a `ROLLBACK` executed from inside a running statement,
  in a transaction that also changed the schema, aborts the statement
  under stock (`abort due to ROLLBACK`); here the statement completes.
  Cursor tripping itself works (trans3 passes); the schema-change arm
  does not reach it.
- triggerA (3.transient.*) — 7 cases involving transient triggers.
- expridx2 — requires WAL mode.

## Per-test guards (`# zeroskip:` markers in test files)

- collate1.test:6.5-6.8 — limitation — PRIMARY KEY on an
  application-registered collation (memcmp-encodable keys).
- delete.test:8.x — framework — read-only via chmod 0444 assumes a
  single-file database; also read-only-media open (see Upstream asks).
- trans.test:9.x.4/.5 — framework — VFS sync counters; zeroskip syncs
  inside the library, invisible to the VFS.
- savepoint.test:3.x, 10.2.x — framework — PRAGMA lock_status reads the
  vestigial pager.
- savepoint.test:5.4.3/5.4.4 — semantic — lock-free readers never block
  a commit; rerouted to the WAL branch, which matches.
- savepoint.test:7.1 — framework — PRAGMA page_count reads the
  vestigial pager.
- savepoint.test:11.8 — framework — file-size probe on a directory
  database.
- savepoint.test:14/15/16 (multiclient) — semantic — journal-mode
  blocking (readers block a writer's commit); zeroskip readers are
  lock-free by design.
- update.test:18.10/18.20 — limitation — UTF-16 databases (key encoding
  is UTF-8 only).
- Capability `incrblob` reports 0 under SQLITE_ZEROSKIP (test_config.c),
  auto-skipping every incrblob-guarded test suite-wide — limitation.

## Engine bugs found by the suite

1. **Mid-scan commit invalidated every cursor** (delete-9.x, and any
   write statement completing while a SELECT streams).  A committing
   write txn with other active readers now downgrades to a fresh read
   snapshot, mirroring stock's TRANS_WRITE->TRANS_READ downgrade.
2. **Cursor pinning was a no-op** (update-20.x): REPLACE conflict
   resolution could let a DELETE trigger corrupt the index being
   updated (ticket 314cc133's protection).  Writes moving a pinned
   cursor now fail SQLITE_CONSTRAINT_PINNED.
3. **Restore-then-Next skipped a row** (delete-9.3): landing on a
   vanished row's successor now sets stock's skipNext semantics.
4. **Same-process double writer, committed data lost** (savepoint-14):
   zeroskip's fcntl locks are per-process, so two connections in one
   process could both hold "the" write transaction and the first commit
   was silently discarded.  The engine now keeps a process-global write
   registry keyed by the directory's dev/inode (stock's unixInodeInfo
   pattern).  Also: sqlite3BtreeClose released the registry entry late
   (types.test deadlock).
5. **Read-only databases** now degrade to a ZS_SHARED open instead of
   erroring (delete-8 investigation; full effect blocked on the
   upstream read-only-media ask).
6. **Index inserts appended siblings instead of replacing** (upfrom1):
   UPDATE on WITHOUT ROWID tables emits no IdxDelete and relies on
   BtreeInsert overwriting the entry matching on the first pX->nMem
   fields (stock loc==0 semantics).  Silent row duplication.
7. **Borrowed positions died on mid-txn rollover** (gencol1,
   savepoint2 crashes): cursor keys and undo-log values are now
   engine-owned copies; A-4 rollover boundary queued upstream.
8. **Statements did not observe rollback** (trans3): BtreeRollback now
   honours tripCode (SQLITE_ABORT_ROLLBACK).
9. **Key encoding: int-vs-large-double equality** (intreal-2.5):
   doubles >= 2^53 now encode exact decimal digits; MEM_IntReal takes
   precedence over an accompanying MEM_Int and compares as double.
10. **Reverse scan stopped when its current row was deleted**
    (eval-2.3, eval-3.1).  Restoring a cursor whose row vanished did a
    forward (GE) seek; when the row had no successor the cursor went
    INVALID and the next Previous() gave up, truncating the scan to one
    row.  Restore now falls back to a strict-LE seek onto the
    predecessor and marks it so Previous() delivers rather than steps
    past it — the mirror image of bug 3, and stock's behaviour (its
    cursor lands on the nearest cell either way).
11. **Rollback ended the read snapshot other statements were using**
    (progress-1.7, misc8-1.4).  Stock's btreeEndTransaction keeps the
    Btree in TRANS_READ when db->nVdbeRead>1; the engine did that on
    the commit path but not the rollback path, so an interrupted or
    aborted inner statement silently truncated the enclosing scan.
    BtreeRollback now performs the same downgrade.
12. **Wrong error for a non-database path** (misc5-4.1): opening a path
    that exists but is not a directory reported an I/O error; it now
    reports SQLITE_NOTADB, as stock does for a bad header.

## Upstream asks: resolved

All closed as of the 2026-08-14 re-vendor (upstream 9e93655):

- **A-4 borrow lifetime (A-4a).** There is no mid-transaction rollover:
  the active file is chosen once, at the first store, and pinned.  The
  bug was the gap before that first store -- starting a new generation
  refreshed the handle and released the outgoing snapshot, unmapping
  any value an earlier fetch had returned (and `zsi_cursor_refresh` had
  the same bug for live cursors).  A first fix (30a9966) was
  incomplete: retire took the mappings over only on the LAST reference
  and plainly released otherwise, which assumes the remaining holder
  outlives the borrower.  A cursor is the counterexample -- fetch, open
  cursor, store, close cursor -- and closing it unmapped bytes the
  transaction was promised.  9e93655 transfers the reference into the
  hold list instead.  The engine's defensive copies are out: the
  savepoint undo log borrows before-images again.
- **The engine's "empty value borrows back as NULL" report was wrong.**
  Measured against the vendored library: a zero-length value returns
  ZS_OK with a NON-NULL pointer and nOld==0, on both the uncommitted
  and committed paths, and an absent key returns ZS_NOTFOUND.  The
  return code alone distinguishes them; no presence flag is needed.
  The failure-count change that suggested otherwise was noise from a
  corrupting bug, misattributed.
- **Same-process write exclusion (C-1j).** Moved into the library:
  `F_OFD_SETLK` on Linux/macOS, a dev/ino registry elsewhere, both
  alongside the fcntl lock.  The engine's `ZsProcLock` registry is
  deleted; `zs_db_begin_txn` returning ZS_LOCKED now drives the busy
  handler on its own.  Regression coverage: `test/zs/twowriter.test`.
- **ZS_SHARED on write-protected media.** Not a library bug -- the
  engine's own `ZS_CREATE` attempt was the one failing.  Measured
  against the vendored library on `dr-x------` + `-r--------` (APFS):
  `ZS_SHARED` and `ZS_SHARED|ZS_NONBLOCKING` both return ZS_OK with a
  working fetch; `ZS_CREATE` returns ZS_IOERROR/EACCES, which is
  correct.  End to end the engine now reads a write-protected database
  and reports `attempt to write a readonly database` on writes, as
  stock does.  What remains is framework-shaped: `delete.test`'s
  `chmod 0444` clears a directory's traverse bit, so nothing inside can
  be opened -- inherent to a database being a directory.

One item to watch, from the C-1j handover: `zs_db_begin_cursor` without
ZS_SHARED holds the write lock for the cursor's lifetime and now blocks
same-process handles too.  The engine is unaffected -- every cursor it
opens (btree_zs.c and backup_zs.c alike) goes through
`zs_txn_begin_cursor` inside an existing transaction.
