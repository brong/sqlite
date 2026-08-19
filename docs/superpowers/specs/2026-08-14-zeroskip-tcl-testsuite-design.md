# Design: SQLite TCL test suite against the zeroskip engine

Date: 2026-08-14
Status: draft, pending review

## Goal

Run SQLite's real TCL test suite against the zeroskip storage engine —
"porting all the tests for real", the BDB-style undertaking the PoC
deliberately deferred.  Success:

- A `testfixturezs` binary linking the non-amalgamation zeroskip
  library builds and runs the TCL framework.
- A `zeroskip` test configuration runs the veryquick-class suite (the
  standard smoke tier, tens of thousands of assertions) with **zero
  unexplained failures**: every excluded file and every skipped test
  carries a one-line justification, and everything not excluded passes.
- Engine bugs the suite finds are fixed as found (that is the point).
- The full-suite tier beyond veryquick is a stretch goal, same rules.

## Why this is BDB-shaped work

A large fraction of the suite tests the storage layer itself — journal
files, WAL, page sizes, corrupt-database fixtures, incrblob, dbstat,
freelists, cell overflow.  Those are meaningless or guaranteed-fail
for a foreign engine regardless of correctness.  The deliverable is
therefore two artifacts of equal weight: the passing runs, and the
**triage ledger** separating "tests the pager, excluded" from "found
an engine bug, fixed" from "engine limitation, documented".

## Architecture

**Build.** `testfixturezs` mirrors the stock `testfixture` target but
links `libsqlite3.a` (non-amalgamation — the only build containing the
engine) instead of `sqlite3.c`.  The library for testing is built with
the same OPTIONS as stock testfixture adds (`SQLITE_TEST`,
`SQLITE_NO_SYNC`, `SQLITE_DEFAULT_PAGE_SIZE=1024`, ...) plus
`SQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE`; test hooks compiled into
the core are required by the framework, so a plain lib will not do.
Two testfixture-only sources poke btree internals and get the same
2-line `#ifndef SQLITE_ZEROSKIP` guards btree.c has: `test_btree.c`,
`test3.c` (their tcl commands serve btree*-internal tests that are
excluded anyway).  Environment note: the system TCL needs
`-L/opt/homebrew/lib` for libtommath; the target hardwires nothing —
a documented make variable covers it.

**Suite configuration.** A `zeroskip` permutation in
`test/permutations.test`, following the existing pattern (`journaltest`,
`inmemory_journal`, ...): a base exclude list of storage-layer test
files, each with a trailing comment naming its reason.  For per-test
granularity inside otherwise-passing files, a tcl capability:
`SQLITE_ZEROSKIP` is added to the `ctime.c` compile-options table
(guarded, zero effect on stock) so tests and the framework can say
`ifcapable !zeroskip { ... }` — the suite's native idiom.  Preference
order when a test misbehaves: fix the engine > exclude the file with a
reason > guard the individual test.  Guards inside test files are kept
to a minimum and marked with a `# zeroskip:` comment so upstream merges
stay tractable.

**Database-shape differences the framework will trip on.** Known ahead
of triage, to be handled in `test/tester.tcl` or the permutation setup
rather than per-test: databases are directories (`file delete -force
test.db` works, `file size test.db` does not); there is no `-journal`
file; `hexio_*`/`sqlite3_dbpage`-style direct file access is
storage-layer by definition; PRAGMA page_size/journal_mode report
fixed answers.

**Triage ledger.** `doc/zeroskip-testsuite.md`: for each excluded file,
one line — category (pager/journal/wal/format/corrupt/incrblob/
dbstat/limitation) and reason.  Engine bugs found and fixed get their
own section with the failing test as the reference.  The ledger is the
document a future "can we trust this engine" question reads.

## Execution shape

1. Build plumbing: guards, `testfixturezs` target, capability flag,
   `zeroskip` permutation with an empty exclude list.
2. First contact: run a handful of core files (select1, insert, index,
   trans, savepoint) directly; fix the inevitable framework breakage
   (directory databases, journal probing in tester.tcl helpers).
3. Sweep: run the veryquick file list under the permutation; bucket
   every failing file — obvious storage-layer files straight to the
   exclude list with reasons, everything else investigated.
4. Grind: for each investigated failure — engine bug (fix, add to
   ledger), semantic difference (guard or exclude, justify), framework
   assumption (patch tester.tcl under the capability).
5. Gate: clean zeroskip-permutation veryquick run; ledger complete;
   full standing verification (batteries, crash/busy/backup, ASan,
   kvbench sanity) still green; stock testfixture veryquick unchanged.

Steps 3-4 are the unknown-size bulk; the plan will structure them as
repeatable triage rounds rather than pretending to enumerate failures
in advance.

## Risks

- Volume: hundreds of test files; triage discipline (categorize fast,
  investigate only the non-obvious) is the schedule.
- `SQLITE_TEST` code paths in core may themselves assume the pager
  (fault-injection hooks, test_superlock, quota/multiplex VFS tests) —
  expect a second round of stubs-or-excludes at that layer.
- tester.tcl edits must not change stock behavior: everything keyed on
  the capability, verified by re-running stock veryquick at the end.
