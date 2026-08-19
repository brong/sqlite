# Design: zeroskip storage engine for SQLite

Date: 2026-08-11
Status: draft, pending review

## Goal

A BerkeleyDB-SQL-style storage engine swap: SQLite's SQL compiler, VDBE, and
everything above `btree.h` stay untouched; the btree layer is reimplemented on
top of zeroskip (`../zeroskip2`), an append-only ordered key-value store with
lock-free snapshot readers and a single writer.

This is a **proof of concept**. Success = real SQL end-to-end on zeroskip —
tables, indexes, WITHOUT ROWID tables, transactions, savepoints, DESC scans,
the online backup API — benchmarkable via `speedtest1`, with the exotic
corners stubbed honestly (loud errors or documented no-ops, never silent
wrong answers).

### Non-goals

- Passing the full SQLite test suite.
- Custom (application-registered) collations in indexes.
- Shared cache, incremental blob I/O, dbstat/dbpage correctness, WAL
  checkpointing (meaningless here), auto/incremental vacuum.
- Two-phase-safe multi-database (ATTACH) commit.

## Architecture and build

- New file `src/btree_zs.c` implements the entire `sqlite3Btree*` API from
  `btree.h`. It is compiled **instead of** `src/btree.c` when `SQLITE_ZEROSKIP`
  is defined. Similarly `src/backup_zs.c` replaces `src/backup.c`.
- zeroskip is **vendored** into `ext/zeroskip/` (`zeroskip.c`, `zeroskip.h`,
  `xxhash.h`), snapshotted from `../zeroskip2` with the source commit hash
  recorded in `ext/zeroskip/VENDOR` for re-syncing. zeroskip2 is under active
  development; re-vendoring is a deliberate, recorded step.
- The path given to `sqlite3_open()` is the zeroskip **directory**. Open flags
  map: CREATE → `ZS_CREATE`, READONLY → `ZS_SHARED`.
- `pager.c`, `pcache*.c`, `wal.c` remain compiled: the pager API leaks into
  ~15 core files (pragma.c, vdbe.c, main.c, vacuum.c, ...). Each zeroskip
  `Btree` owns a vestigial in-memory `Pager` solely so `sqlite3BtreePager()`
  returns something real and those call sites keep functioning. It stores no
  data.
- The stock build is untouched when `SQLITE_ZEROSKIP` is not defined.

## Data layout

One zeroskip database holds all of a SQLite database's trees, under the
**default byte-order comparator**. One `zs_txn` therefore covers a whole SQL
transaction atomically, and `zstool` works on the result.

Every zeroskip key is `[tree-id][encoded key]`:

- **Tree ids**: 4-byte big-endian, allocated from a counter in the meta space.
  They flow through the existing `Pgno` plumbing unchanged ("root page"
  numbers). Id 1 is `sqlite_schema`, matching SQLite's convention. Id 0 is
  reserved for meta.
- **Rowid tables** (`BTREE_INTKEY`): encoded key = rowid as 8-byte big-endian
  with the sign bit flipped (memcmp order == signed order). Value = the row
  record.
- **Index trees** (`BTREE_BLOBKEY`, used by indexes and WITHOUT ROWID tables):
  encoded key = SQLite4-style order-preserving encoding of the index record:
  NULL < numbers < text < blob; int and real collapse into one sortable
  numeric form; text is encoded per collation (BINARY, NOCASE, RTRIM only);
  DESC columns are byte-inverted; a terminator keeps prefix keys ordered
  before their extensions. Value = **the original record bytes**, so reads
  never decode — `sqlite3BtreePayload*` returns the stored record directly.
  Creating an index with a custom collation fails with a clear error.
- **Meta**: the 16 `sqlite3BtreeGetMeta`/`UpdateMeta` slots (schema cookie,
  file format, text encoding, user version, application id, ...) and the
  tree-id counter live under tree-id 0.

Deletes are real zeroskip deletes (NULL-value stores). The distinct
empty-value-vs-absent state is not relied upon.

## Transactions, savepoints, locking

- SQLite read transaction → `zs_db_begin_txn(shared=1)`: a pinned snapshot,
  lock-free. Write transaction → exclusive zs write txn. `ZS_NONBLOCKING` is
  used and `ZS_LOCKED` maps to `SQLITE_BUSY`, so busy handlers and timeouts
  work. Single-writer / many-lock-free-readers matches SQLite's
  rollback-journal concurrency model; multiple connections and processes work
  without extra code.
- **Read→write upgrade**: zeroskip has no txn upgrade, so
  `BtreeBeginTrans(wrflag=1)` inside an open read txn aborts and re-begins.
  This is the one place cursor positions are copied to the heap (see Cursors).
  The re-begin re-snapshots; SQLite's schema-cookie check
  (`BTREE_SCHEMA_VERSION` meta) already handles cross-snapshot schema change
  detection, same as stock.
- `CommitPhaseOne` = no-op; `CommitPhaseTwo` = `zs_txn_commit`. Multi-database
  ATTACH commit is therefore not atomic across files — documented limitation.
- **Savepoints / statement journals**: emulated with an in-backend undo log
  over zeroskip's flat transactions. Every write funnels through one backend
  store function; while any savepoint is open it first fetches the key's
  before-image and appends (key-copy, borrowed value-pointer) to the undo log
  — value pointers are txn-lifetime per A-4, and the undo log dies with the
  txn, so no value copies. `ROLLBACK TO` replays before-images in reverse.
  `sqlite3BtreeSavepoint`/`BeginStmt` manage undo-log watermarks.
- `zs_db_should_repack`/`zs_db_repack` run opportunistically after commit.
  `VACUUM` works two ways: the stock `vacuum.c` path (via the backup engine's
  `sqlite3BtreeCopyFile`) and, preferred, `zs_db_compact`.

## Cursors — zero-copy

A `BtCursor` wraps an open zs cursor plus a borrowed `(key, keylen)` current
position. Per zeroskip spec rule **A-4**, pointers returned by a txn or cursor
stay valid for that object's lifetime, which gives:

- **Invariant**: a `BtCursor` holding a position always holds an open zs
  cursor, so its borrow is always covered. Rowid-table cursors don't borrow —
  the rowid decodes into an `i64` field.
- **Seeks**: GE/GT via fetch / `ZS_FETCHNEXT`; LE/LT, `Last`, `Previous` via
  zeroskip reverse support (see Dependencies). `ZS_CURSOR_PREFIX` on the
  tree-id prefix guarantees iteration never leaves its tree.
- **Re-seek without copying**: on direction change or post-write
  invalidation (any write bumps an epoch; stale cursors lazily reposition),
  the replacement zs cursor is opened *first*, passing the borrowed key as
  the seek argument, then the old cursor is closed — both alive during the
  call, so the borrow is valid exactly when used.
- **Zero-copy reads**: `sqlite3BtreePayloadFetch` hands the VDBE zeroskip's
  mmap'd value pointer directly (SQLite's until-cursor-moves contract is a
  subset of A-4's cursor-lifetime contract).
- **The one copy path**: read→write txn upgrade kills every zs cursor at
  once; each open cursor's position is copied to the heap, cursors are
  recreated in the new txn, copies freed.
- `BTREE_FORDELETE` cursors are no-ops (the hint exists for engines like
  this). `BtreeCount` is a prefix scan. `sqlite3BtreeLast` on a rowid table
  is a reverse-seek on the tree prefix (rowid allocation on INSERT).

## Backup API

`src/backup_zs.c` reimplements `sqlite3_backup_init/step/remaining/pagecount/
finish` and `sqlite3BtreeCopyFile`. A zeroskip shared txn *is* the stable
snapshot stock backup.c labors to fake:

- First `step`: shared txn on source, write txn on destination, clear the
  destination (cursor-walk + tombstone deletes; compaction reclaims).
- Each `step(n)`: copy a batch of records proportional to n via a
  full-keyspace cursor, borrowed
  pointers feeding `zs_txn_store` directly.
- `finish`: commit destination, end source txn. `remaining`/`pagecount`
  report record-based numbers from one counting pre-scan (advisory values).
- **Semantic deviation (improvement)**: concurrent source writes never
  invalidate or restart the backup; the result is consistent as of the first
  step. Same-connection writes do not propagate mid-flight as stock's do.

## Stubs and error mapping

Stubbed, failing loudly: incremental blob I/O (`SQLITE_ERROR`), shared-cache
table locks (single-connection semantics preserved), `BtreeCheckpoint`
(no-op), auto/incremental vacuum (no-op), page-size/cache-size/spill/mmap
knobs (accepted, ignored), `dbstat`/`dbpage` (operate on the empty vestigial
pager; documented nonsense).

`PRAGMA integrity_check` maps to `zs_db_check_consistency` plus a per-tree
walk validating that every record decodes and that table/index entry counts
match (using the `aRoot`/`aCnt` interface of `sqlite3BtreeIntegrityCheck`).

Error mapping: `ZS_LOCKED` → `SQLITE_BUSY`; `ZS_NOTFOUND` → not-found seek
results (not errors); `ZS_READONLY` → `SQLITE_READONLY`; `ZS_BADFORMAT`/
`ZS_BADCHECKSUM` → `SQLITE_CORRUPT`; `ZS_FULL` → `SQLITE_FULL`; `ZS_IOERROR`
→ `SQLITE_IOERR`; anything else → `SQLITE_INTERNAL` (uniqueness is enforced
above the btree layer, so the conditional-store flags and `ZS_EXISTS` are
not used).

## Dependencies on zeroskip2 (handed off)

Blocking: **reverse iteration**, as specified in the handoff note —
predecessor fetch (largest key ≤ K, and strictly < K) on db and txn forms
seeing own uncommitted writes; reverse cursors on `zs_db_begin_cursor`/
`zs_txn_begin_cursor` honoring empty-start-=-last, `ZS_SKIPROOT`,
`ZS_CURSOR_PREFIX`, snapshot semantics, and write-through. Not needed:
bidirectional cursors, reverse foreach, reverse+`ZS_CURSOR_LIVE`.

Required and believed already true (state as requirements):

- Multiple concurrent cursors on one transaction.
- Writes through a txn while its cursors are open, with A-4 pointer validity
  undisturbed (`INSERT INTO t SELECT * FROM t`).
- Reverse cursors inherit A-4 unchanged.

Until reverse support lands in the vendored snapshot, `Last`/`Previous` use a
temporary O(n) forward-scan emulation so everything else can be built and
tested.

## Testing

1. **Key-encoding unit harness** (C): the one genuinely tricky pure function.
   Property test: for random record pairs, memcmp of encodings agrees with
   `sqlite3VdbeRecordCompare` for supported collations, including DESC and
   prefix cases.
2. **End-to-end SQL battery** through the shell: DDL, CRUD, indexes, DESC
   scans, WITHOUT ROWID, savepoints and statement rollback, multi-statement
   transactions, concurrent reader-while-writing, backup API, and
   crash-recovery (kill -9 mid-write, reopen, integrity_check).
3. **Benchmarks**: `speedtest1` built with `SQLITE_ZEROSKIP` vs stock.
4. Stretch, not a gate: a curated slice of the TCL suite.

## Risks / open questions

- **Key-encoding correctness** is the highest-risk component; hence the
  property test against `sqlite3VdbeRecordCompare` as ground truth.
- The vestigial-pager trick assumes no core path *writes* through the pager
  outside btree.c/backup.c; if one surfaces, stub that call site.
- zeroskip2 is mid-change; vendoring pins us to a snapshot and re-vendoring
  is manual by design.
- Large transactions: the undo log and zeroskip's txn write buffering are
  both memory-resident; a giant UPDATE inside a savepoint is bounded by RAM.
  Acceptable for PoC.
