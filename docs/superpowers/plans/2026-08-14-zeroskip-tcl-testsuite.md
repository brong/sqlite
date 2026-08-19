# Zeroskip TCL Test Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** SQLite's TCL test suite runs against the zeroskip engine: a `zeroskip` permutation passes the veryquick-class tier with zero unexplained failures, backed by a triage ledger.

**Architecture:** `testfixture` already links `libsqlite3.a` when `USE_AMALGAMATION=0`, so the zeroskip test build is the existing target under the engine's OPTIONS (binary copied aside as `testfixturezs`). A `zeroskip` entry in `test/permutations.test` holds the exclude list; `SQLITE_ZEROSKIP` becomes a reported compile option so `ifcapable !zeroskip` works natively. The bulk of the work is repeatable triage rounds, not enumerable up front.

**Tech Stack:** TCL test framework (`test/tester.tcl`, `test/permutations.test`), main.mk, tool/mkctime.tcl.

**Spec:** `docs/superpowers/specs/2026-08-14-zeroskip-tcl-testsuite-design.md`

## Global Constraints

- Preference order per misbehaving test: fix the engine > exclude the file with a reason > guard the single test (`# zeroskip:` marker).
- Every exclusion/guard gets a category + one-line justification in `doc/zeroskip-testsuite.md` (categories: pager/journal/wal/format/corrupt/incrblob/dbstat/limitation/framework).
- tester.tcl changes keyed on the zeroskip capability only; stock veryquick must still pass at the end.
- Zeroskip test builds: `make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' 'LDFLAGS.libsqlite3=-L/opt/homebrew/lib -rpath /usr/local/lib -lz' testfixture && cp testfixture testfixturezs`. Stock rebuilds use the same command without the ZEROSKIP OPTIONS; the two share object files, so full re-verify after switching.
- Long suite runs wrapped in `caffeinate -i`.
- Commit at every green state; engine bugs found get their own commits with the discovering test named.

---

### Task 1: Zeroskip testfixture builds and runs one real test file

**Files:**
- Modify: `main.mk` (only if the build reveals a needed rule change), `src/test_btree.c`, `src/test3.c` (2-line `#ifndef SQLITE_ZEROSKIP` guards, only if compile/link demands them)
- Create: `testfixturezs` (build artifact, not committed)

**Interfaces:**
- Produces: `./testfixturezs <testfile>` runs TCL tests against the zeroskip engine; the exact build command (recorded in Global Constraints) for later tasks.

- [ ] **Step 1: Attempt the build**

Run:
```bash
make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
     'LDFLAGS.libsqlite3=-L/opt/homebrew/lib -rpath /usr/local/lib -lz' \
     lib testfixture 2>&1 | grep -iE "error|undefined" | head -20
```
Expected: either a clean link, or compile/link errors from testfixture-only sources poking btree internals (`test_btree.c`, `test3.c` are the known candidates; the amalgamation-only assumption in others is possible).

- [ ] **Step 2: Guard whatever broke**

For each failing testfixture source: add the same 2-line guard btree.c carries —
```c
#ifndef SQLITE_ZEROSKIP /* pokes stock btree internals */
...existing file...
#endif /* !defined(SQLITE_ZEROSKIP) */
```
If a guarded file leaves a tcl command referenced by `sqlite3TestInit` registration tables elsewhere, guard the registration call site the same way. Rebuild until it links.

- [ ] **Step 3: Smoke a core test file**

Run: `cp testfixture testfixturezs && ./testfixturezs test/select1.test 2>&1 | tail -5`
Expected: a summary line (`N errors out of M tests`). ANY completion — even with failures — is success for this task; failures are Task 3/4 material. Record the count.

- [ ] **Step 4: Verify the stock build still works after guards**

Run:
```bash
make USE_AMALGAMATION=0 'LDFLAGS.libsqlite3=-L/opt/homebrew/lib -rpath /usr/local/lib -lz' lib testfixture >/dev/null 2>&1 \
  && ./testfixture test/select1.test 2>&1 | tail -2
```
Expected: `0 errors out of ...`.

- [ ] **Step 5: Commit** — `git add -A src main.mk && git commit -m "zeroskip testfixture builds; guards on btree-internal test sources"`

---

### Task 2: Capability flag and the zeroskip permutation skeleton

**Files:**
- Modify: `tool/mkctime.tcl` (add `SQLITE_ZEROSKIP` to the boolean options list, alphabetical position), `test/permutations.test`, `test/tester.tcl` (capability-keyed helpers only)
- Create: `doc/zeroskip-testsuite.md` (ledger skeleton)

**Interfaces:**
- Produces: `ifcapable zeroskip {...}` / `ifcapable !zeroskip {...}` work in any test file; `./testfixturezs test/permutations.test zeroskip [file...]` runs files under the permutation; ledger file with section headers (Excluded files / Per-test guards / Engine bugs found / Upstream asks).

- [ ] **Step 1: Add the compile option**

In `tool/mkctime.tcl`'s boolean-options list add `ZEROSKIP` following the existing pattern (the generator emits `#ifdef SQLITE_ZEROSKIP "ZEROSKIP",` style entries). Rebuild; verify:
```bash
./testfixturezs <<'EOF'
sqlite3 db :memory:
puts [db eval {SELECT sqlite3_compileoption_used('ZEROSKIP')}]
EOF
```
Expected: `1` (and `0` from stock testfixture).

- [ ] **Step 2: Add the permutation**

In `test/permutations.test`, following the `journaltest` pattern:
```tcl
test_suite "zeroskip" -description {
  Run tests against the zeroskip storage engine (SQLITE_ZEROSKIP).
  Excluded files test the stock pager/btree storage layer itself;
  doc/zeroskip-testsuite.md carries one justification line per entry.
} -files [test_set $::allquicktests -exclude {
  wal* pager* journal* crash* corrupt* incrblob* incrvacuum* dbstat*
  dbpage* backup* mmap* superlock* snapshot* vacuum* io.test ioerr*
  bigfile.test avfs.test stmtvtab1.test
}]
```
This initial list is a starting hypothesis, not the ledger — Task 4 refines it in both directions (backup*/vacuum* may come back if they pass; more may leave).

- [ ] **Step 3: Ledger skeleton**

`doc/zeroskip-testsuite.md` with the four section headers and the run command. Commit both: `git commit -m "zeroskip test permutation skeleton, ZEROSKIP compile option, ledger"`.

---

### Task 3: First contact — core files pass individually

**Files:**
- Modify: `test/tester.tcl` (capability-keyed), `src/btree_zs.c` / other engine files (bug fixes), individual `test/*.test` only as a last resort

**Interfaces:**
- Consumes: `testfixturezs`, capability from Tasks 1-2.
- Produces: these files pass standalone: `select1 select2 select3 insert insert2 update delete index index2 trans savepoint types types2 collate1 where distinct orderby1 default func`.

- [ ] **Step 1: Run the batch, capture per-file results**

```bash
for f in select1 select2 select3 insert insert2 update delete index index2 \
         trans savepoint types types2 collate1 where distinct orderby1 default func; do
  r=$(./testfixturezs test/$f.test 2>&1 | grep -E "errors out of" | head -1)
  echo "$f: ${r:-CRASHED}"
done
```

- [ ] **Step 2: Triage loop over failures (repeat until batch is clean)**

For each failing file, in this order:
1. Read the first failing test's output (`./testfixturezs test/<f>.test 2>&1 | grep -B2 -A8 "^! " | head -40`).
2. Classify: engine bug → fix in engine code with the standing verification suite re-run (`./test/zs/run-tests.sh . && ./zskey-test && ./zsbtree-test`); framework assumption (file size/journal probing/path shape) → fix in tester.tcl under `ifcapable zeroskip`; genuine semantic difference (fixed page_size answers, journal_mode reporting, database-is-a-directory) → `# zeroskip:` guard on the specific test.
3. Ledger every guard and every bug.
4. Commit each green file-batch: `git commit -m "zeroskip suite: <files> pass (<n> engine fixes, <m> guards)"`.

Known-ahead framework candidates (check before hunting): `file size test.db`, `file exists test.db-journal`, `forcedelete` on directories, `db eval {PRAGMA page_size}` assumptions in tester.tcl procs.

- [ ] **Step 3: Commit the clean batch state**

---

### Task 4: Sweep and triage rounds until the tier is clean

**Files:**
- Modify: `test/permutations.test` (exclude list, each entry justified in the ledger), `doc/zeroskip-testsuite.md`, engine sources (fixes), test files (rare guards)

**Interfaces:**
- Consumes: everything above.
- Produces: `caffeinate -i ./testfixturezs test/permutations.test zeroskip` completes with 0 errors.

- [ ] **Step 1: Full sweep, harvested per file**

```bash
caffeinate -i ./testfixturezs test/permutations.test zeroskip 2>&1 | tee /tmp/zs-sweep.log
grep -E "errors out of|Failures on these tests" /tmp/zs-sweep.log | tail -5
```
If the run aborts on a crashing file, note it, exclude it temporarily (marked `# zeroskip: TRIAGE crash` in the list), and restart the sweep — crashes get investigated in Step 2 with the highest priority since they may be engine memory bugs.

- [ ] **Step 2: Triage round (repeat until sweep is clean)**

Bucket every failing/crashing file from the sweep:
- **Storage-layer by inspection** (reads raw file bytes, drives the pager, journal/wal semantics, page-size math): straight to the exclude list + one ledger line. No investigation beyond the first screenful.
- **Crash or ASan-suspicious**: reproduce standalone; if engine memory bug, fix and re-run the standing verification incl. an ASan build of the batteries; ledger under Engine bugs.
- **Wrong results / errors in plain SQL**: engine bug until proven otherwise. Fix, ledger, commit with the test name.
- **Needs upstream zeroskip change**: exclude + ledger under Upstream asks (do not block on it).
- **Unsupported-by-design** (incrblob, custom collations, ATTACH two-phase, serialize): exclude/guard + ledger under limitation.
Commit at the end of each round: `git commit -m "zeroskip suite triage round N: X fixed, Y excluded, Z guarded"`.

- [ ] **Step 3: Reinstatement pass**

Try removing exclude-list entries that were hypotheses rather than verdicts (`backup*`, `vacuum*`, any `TRIAGE` marker): the engine supports backup and VACUUM, so those *should* pass. Anything that passes leaves the list; anything that stays gets its real justification.

- [ ] **Step 4: Clean-run gate**

`caffeinate -i ./testfixturezs test/permutations.test zeroskip` → 0 errors, no `TRIAGE` markers left, every exclusion ledgered. Commit.

---

### Task 5: Stretch tier, stock re-verify, ledger finish

**Files:**
- Modify: `doc/zeroskip-testsuite.md`, `doc/zeroskip-engine.md` (testing section points at the suite), memory file

- [ ] **Step 1: Stretch — wider file set**

Time permitting: extend the permutation's base set from `$::allquicktests` toward the fuller list (`$::alltests`) and run one sweep; triage by the Task 4 procedure but timebox to one round, ledgering the frontier ("full-tier status" section) rather than grinding to clean.

- [ ] **Step 2: Standing verification, both engines**

```bash
./test/zs/run-tests.sh . && ./zskey-test && ./zsbtree-test && \
./test/zs/crash-test.sh . && ./test/zs/busy-test.sh . && ./test/zs/backup-concurrent.sh .
# then rebuild stock and confirm the framework edits changed nothing:
make USE_AMALGAMATION=0 'LDFLAGS.libsqlite3=-L/opt/homebrew/lib -rpath /usr/local/lib -lz' lib testfixture
caffeinate -i ./testfixture test/veryquick.test 2>&1 | tail -3
```
Expected: all green; stock veryquick 0 errors.

- [ ] **Step 3: Finish the ledger and docs; final commit**

Ledger gets the summary header (files run/passed/excluded by category, engine bugs found+fixed, upstream asks). `doc/zeroskip-engine.md`'s Testing section gains the permutation run line. Commit.

## Plan self-review notes (resolved inline)

- Spec coverage: build (T1), capability+permutation+ledger (T2), first contact (T3), sweep/triage/gate (T4), stretch+stock-unchanged+ledger (T5). Framework database-shape items are T3 Step 2's known-ahead list.
- The exclude list in T2 is explicitly a hypothesis; T4 Step 3 forces reinstatement so the ledger never contains unexamined excludes.
- Object-file sharing between stock and zeroskip builds means every engine-switch rebuild is full; the plan sequences stock verification last to avoid thrash.
