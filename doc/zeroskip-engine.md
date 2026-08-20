# The zeroskip storage engine (SQLITE_ZEROSKIP)

A proof-of-concept storage engine swap in the style of BerkeleyDB's SQL
interface: SQLite's SQL compiler, VDBE, and everything above `btree.h`
are untouched, while the btree layer is reimplemented on
[zeroskip](../ext/zeroskip/) — an append-only ordered key-value store
with lock-free snapshot readers and a single writer.

Design: `docs/superpowers/specs/2026-08-11-zeroskip-btree-engine-design.md`
Plan: `docs/superpowers/plans/2026-08-11-zeroskip-btree-engine.md`

## Building

Non-amalgamation builds only:

```
./configure
make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
     lib sqlite3zs
./sqlite3zs /path/to/database.zs 'CREATE TABLE t(a,b); ...'
```

The path passed to `sqlite3_open()` is a zeroskip **directory** (created
on first write).  `SQLITE_OMIT_SHARED_CACHE` is required; btree_zs.c
enforces it.  The stock build is unaffected — `src/btree.c` and
`src/backup.c` carry only a 2-line `#ifndef SQLITE_ZEROSKIP` guard each.

## What works

- Tables, indexes (incl. multi-column, UNIQUE, DESC columns, partial),
  WITHOUT ROWID tables, `sqlite_schema`, ATTACH, temp/ephemeral tables.
- Transactions with SQLite's usual semantics: many lock-free snapshot
  readers, one writer, `SQLITE_BUSY` + busy-handler/timeout on writer
  contention.  Multiple connections and processes.
- Savepoints, statement rollback, transaction savepoints (emulated with
  an in-engine before-image undo log over zeroskip's flat transactions).
- `ORDER BY ... DESC`, `max()`, LE/LT seeks via zeroskip reverse cursors.
- The online backup API (`sqlite3_backup_*`, shell `.backup`) — a
  zeroskip shared transaction is a stable snapshot, so concurrent writes
  to the source never restart the backup (a deliberate improvement on
  stock semantics), and an unfinished backup leaves the destination
  untouched.  `VACUUM` works through the same machinery.
- `PRAGMA integrity_check` / `quick_check`: zeroskip consistency check
  plus a full keyspace walk validating per-tree entry counts.
- Crash safety: kill -9 mid-transaction leaves the database intact and
  all-or-nothing (`test/zs/crash-test.sh`).

## Key layout

Everything lives in ONE zeroskip database under the default byte-order
comparator; every key is `[4-byte BE tree-id][payload]`:

- Tree 0 is meta: `[0,0,0,0,'M',idx]` for the 16 btree meta slots,
  `[0,0,0,0,'T']` for the tree-id allocation counter.
- Rowid tables: payload = rowid, 8-byte BE with the sign bit flipped;
  value = the row record.
- Index trees: payload = an order-preserving encoding of the index
  record (see `src/zskey.h` for the exact format: decimal-mantissa
  numerics so `1` and `1.0` encode identically and 64-bit integers stay
  exact, NUL-escaped text/blob, collation transforms, DESC inversion);
  value = the original record, so reads never decode.
  `test/zskey-test.c` property-tests the encoding against
  `sqlite3VdbeRecordCompare` on 50k random record pairs.

Cursors are zero-copy: positions and payloads are borrowed mmap
pointers under zeroskip's A-4 pointer-lifetime rule; re-seeks open the
replacement cursor before closing the one whose borrow seeds it.

### Read-after-write, and why rowid allocation probes

Zeroskip's writer batches records into an append buffer -- 64KB, growing on
demand to 4MB since library 2.6.0 -- but a read returns a pointer into an
mmap, so any read that needs bytes still in the buffer must write() the
buffer to the file first -- for VISIBILITY,
not for A-4's pointer lifetime.  On a bulk load that is three write()
syscalls per record, because SQLite reads before every insert.

`ZS_EPHEMERAL` (A-4b) lets such a read be answered out of the buffer, at
the price of pointers that live only until the engine's next call on
that transaction.  `zsbtPointFetch(..., bProbe)` uses it for
`sqlite3BtreeLast`, which is rowid allocation: `OP_NewRowid` takes the
integer key and nothing else.  100k-row bulk insert: 0.78s -> 0.40s.

The probe keeps a REAL position -- the key is copied into aSavedKey and
pKey addresses that copy -- and defers only the value, which
`zsbtLoadValue` re-reads durably if a payload accessor asks.  An earlier
attempt left the cursor merely "needing a re-seek" instead, which
segfaulted three files and silently corrupted rowids in nine more:
`sqlite3BtreeLast` also serves `OP_Last`, and a cursor without a
position cannot iterate backwards from it.  The tier caught it; a spot
check would not have.

### Reads before writes, measured

zeroskip does NOT write through per record: it buffers (64KB, growing to
4MB since 2.6.0) and writes at commit -- one write() per ~310 records when
that measurement was taken at the fixed 64KB, and a 1000-record transaction
now stays buffered entirely and is written once together with its
terminator.  What made a bulk insert look like write-through was this engine
-- SQLite reads before every insert, and a read needing buffered bytes
flushes the chunk.  Counted on one 100k-row transaction (Linux, strace):

    before ZS_EPHEMERAL   3.00 write() per record, 10 fdatasync total
    after                 1.00 write() per record, 10 fdatasync total

The remaining one per record was not the savepoint undo log: skipping
its before-image fetch entirely (via the append bound) changed neither
the count nor the time, so that experiment was reverted rather than kept
as unpaid complexity.  Counting fetches by engine call site settled it
-- our layer made one durable fetch and ten GetMeta calls in the whole
transaction, so the flushing read was INSIDE the library.  It was the
ancestor decision every store paid before it could encode a record;
upstream removed it (37e4d55) and with it the last read on this path.

The append-only trade is real but separate: on repeated overwrites of
the same key, the btree coalesces many updates into one page write while
zeroskip appends every version and reclaims at repack.  A bulk insert of
distinct keys does not exercise it.

### Scans: where the time goes, and what does not help

Profiled on a 200k-row scan (laptop, vendor 37e4d55): VDBE ~40%, the
library's cursor step ~35%, this engine's glue ~13%, memmove ~8%.  Per
row that is roughly 0.061us in the library (raw zeroskip's own scan
rate), ~0.08us of VDBE, and ~0.03us of glue -- against stock's 0.098us
for the whole thing, because a page walk is nearly free next to a merge
across arms.

Two plausible savings in our glue were measured and are NOT worth
taking:

- `zs_nocsum=1` buys 2.8% (22.8M -> 23.4M rows/s).  XXH3 looks like 13%
  of the profile, but F-5e narrows ZS_NOCSUM to RECORD checksums while
  span verification always runs, so most of it is not skippable.
- Removing the per-row key mirror buys nothing (22.5M vs 22.8M), the
  same answer as when it was measured against the older library.

What does help is fewer arms.  `VACUUM` compacts, and on 200k rows that
is 1.04s for +28% on scans (22.2M -> 28.5M) and +45% on fetches
(349838 -> 508769).  Upstream's raw numbers agree: 25.9M compacted
against 16.4M not.  For a read-heavy deployment that is the lever, and
it is explicit rather than automatic because compaction is unbounded --
it rewrites the whole database in one call while writers continue.

### ZS_IFCHANGED is deliberately unused

The flag skips a store whose value already matches, but deciding costs a
point lookup -- and a lookup per store is exactly what ZS_EPHEMERAL, the
cheap miss and the ancestor removal took OFF this write path.  Paying it
back to avoid a write is the wrong trade for an append-only store, where
the duplicate costs space until the repacker reclaims it and nothing
more.  It would suit a caller that knows it rewrites identical values;
SQLite mostly does not, and the engine cannot tell in advance which
statements would.

### Repacking is the library's job

The engine leaves D-16e's cascade armed and does nothing itself except
`zs_db_compact` when VACUUM asks.  It used to repack after every commit
where `zs_db_should_repack()` was true -- that is `repack_select >= 2`,
so nearly every commit that rolled a generation -- which was scaffolding
from the first engine commit rather than a decision, and it spent write
throughput on a read latency nothing had asked for.  The library's
trigger is narrower (only when a transaction is about to start a new
generation) and better placed (at BEGIN, where nothing is held yet).  The
reason given here used to be structural -- "C-1d orders repack before write, so
the merge cannot hold the write lock" -- and it died when C-1d reversed to
write -> repack, since a commit can now take the repack lock in order and
C-1l's compacting seal does.  The reason that survives is that **the cascade is
unbounded** (D-16b): taking repack inside a commit would hold the WRITE lock
across an unbounded merge and block every other writer for its duration.  At
BEGIN nothing is held, so the merge runs under the repack lock alone.
Measured, the change is neutral within noise on stores, fetches and
scans; what it buys is one policy instead of two.  File counts stay
bounded -- 20000 single-row commits leave three data files, re-checked on
library 2.1.2, whose 2.1.0 release notes warn that repack now "merges
sooner and leaves fewer files".

`zs_open_data.repack_max_size` (new in 2.1.1, default 512MB) bounds what
one merge rewrites, trading a shorter pause for a file count that grows
with the database and a read path that degrades linearly in it.  We leave
it at the default, for the same reason we leave the cascade armed: only one
policy, and it is the library's.  Upstream's answer to "can a bulk load
stop repacking mid-flight" was bound-or-tier, never disarm.

### The pointer-table cache is off by default

`zs_index=local` caches pointer tables in `zeroskip.cache` inside the
database directory; `zs_index_dir=PATH` puts them elsewhere (mutually
exclusive).  Neither is the default, which is a measured choice rather
than a cautious one.

The cache bounds snapshot-open cost, which is the replay of the UNSEALED
TAIL of the active file.  **The 43x and 33x this section used to quote for
that are retired**: both came from fixtures in which the cache itself
prevented the file from sealing, so the cache was removing a cost it had
created.  Upstream confirmed the mechanism and pinned the bound; the
honest numbers are in "What the pointer table is really worth" below --
about 1.2x for one-row commits and 13-15x for batched ones.  The
write-side figures here (20-30% on multi-row stores) are also obsolete:
since library 2.5.0 a commit publishes nothing, and the table's write cost
on production is 0-1%.

Upstream's publish-threshold sweep used to be quoted here for where that
write cost lives.  **It is retired too, and for the same reason as the 43x**:
every threshold in it was publishing a 4.3KB table over an 87KB tail, so the
sweep never reached the "too low costs the writer" end at all -- and since
2.5.0 the store column is flat by construction, because a sole writer
publishes only at its own open.  A single-process harness cannot reach that
end any more.

The cache is therefore now described by the condition that governs it rather
than by a headline multiple.  Enable it when the deployment opens far more
often than it writes -- which is Cyrus, and which our own numbers support at
0-1% write cost for 1.2-1.4x on open.  `zs_index_dir=PATH` on tmpfs is the
shape Cyrus will actually use, so the table costs no pool I/O at all; it is
volatile, and the first opener after a reboot rebuilds it, which is sound
because every table rejection is `ZS_NOTFOUND` rather than an error.

### Clean-room protocol

The production matrix below is the reference; the laptop matrix after it
was measured on a shared machine and is indicative only.  For numbers
worth quoting, run on a quiet machine:

```
# 1. quiesce: no editors, no browsers, no sync daemons, no VMs, no
#    backup or indexing jobs.  On Linux also:
#      sudo cpupower frequency-set -g performance
#      sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
# 2. build and run, capturing the environment with the results
git rev-parse HEAD                   > /tmp/zsbench-env.txt
sed -n 's/^source: //p' ext/zeroskip/VENDOR >> /tmp/zsbench-env.txt
uname -a                            >> /tmp/zsbench-env.txt
lscpu | head -20; free -g           >> /tmp/zsbench-env.txt
zfs get -o property,value all POOL/DATASET >> /tmp/zsbench-env.txt
zpool status                        >> /tmp/zsbench-env.txt
./configure
./test/zs/kvbench-all.sh --dir /POOL/DATASET/bench -n 200000 --reps 5 \
  2>&1 | tee /tmp/zsbench-run1.txt
./test/zs/kvbench-all.sh --dir /POOL/DATASET/bench -n 200000 --reps 5 \
  --rowid 2>&1 | tee /tmp/zsbench-run1-rowid.txt
```

Push the branch BEFORE sending anyone these instructions, and read the
first two lines of the env capture before reading the numbers: a run
that silently benchmarked the previous commit looks exactly like a run
that found nothing.  That has happened once here.  The dataset's
`recordsize` in the same capture is the other line to actually read --
a dataset named for one recordsize and created with another is a matrix
column in the wrong place.

**If you read a syscall count, pass `strace -c -w`.** Plain `strace -c`
summarises SYSTEM time, so a blocking `fdatasync` reports the CPU it burned
and not the time the caller waited -- 12us for a gate whose real latency is
86us.  A prediction was built on that here and was wrong by an order of
magnitude.  The paired durable/`zs_nosync` rows of the matrix measure wall
time by construction and are the cross-check.

Run it three times and keep all three: the durability-bound rows (1 and
10 stores per transaction) are the ones that move, and a single run
cannot show you whether a difference is real.  Record the ZFS
properties that actually bear on the comparison -- `recordsize`,
`compression`, `sync`, `logbias`, `primarycache`, `atime` -- and
whether the pool has a separate intent-log device, because the fsync
path is exactly what the 1-per-txn row measures.

Two ZFS-specific cautions.  First, leave `sync=standard`: `sync=disabled`
turns every engine's durability story into the same story and makes the
comparison meaningless (if you want a reduced-durability number, the
`zs_nosync` row already provides one, and it is honest about what it
gives up).  Second, the default 128K `recordsize` should favour
zeroskip's append-only writes over the btree's 4K scattered page writes;
the matrix below is at `recordsize=4K`, the setting that ought to favour
the btree, so it is the conservative reading -- and measured, the two
barely differ at this record count (see below).
The 1-and-10-per-txn rows are where storage hardware shows up; the
1000-per-txn row is the CPU-bound engine comparison.

### Production matrix (2026-08-19, stl-imap-09)

The reference numbers.  A quiesced all-NVMe production IMAP server (Debian
6.12, AMD EPYC 7402P, 993GB RAM; ZFS `tank` = two 12-wide raidz2 vdevs of
Intel P4510 8TB, `compression=zstd`, `encryption=aes-256-gcm`,
`sync=standard`, `logbias=latency`, **no separate log device**,
`atime=off`).  Everything else stopped, caches dropped.  Scratch datasets,
not the live mail store, and their real `recordsize` read back from `zfs
get` rather than taken from the dataset name -- a run was wasted on that.
20k records, 100-byte values, best of 5, library 2.7.0 (09c1c57), produced by
`test/zs/prodrun.sh`.  Engine-side commit hashes in this document refer to the
pre-squash history, kept on the `zeroskip-engine-history` ref; the working tree
they describe is this one.

Durable at every commit, SQL on zeroskip against stock in default journal
mode, both recordsizes:

| workload            | recordsize | zs (rowid) | stock  | ratio | zs (WOR) | stock  | ratio |
|---------------------|-----------:|-----------:|-------:|------:|---------:|-------:|------:|
| store, 1 per txn    |         4K |    10203/s | 2029/s | 5.03x |   9725/s | 1914/s | 5.08x |
| store, 1 per txn    |       128K |     9880/s | 2170/s | 4.55x |   9564/s | 1966/s | 4.86x |
| store, 10 per txn   |         4K |    85283/s |  18454 | 4.62x |    74609 |  14984 | 4.98x |
| store, 10 per txn   |       128K |    85408/s |  19034 | 4.49x |    78636 |  15606 | 5.04x |
| store, 100 per txn  |         4K |   361193/s | 154119 | 2.34x |   241488 | 108275 | 2.23x |
| store, 100 per txn  |       128K |   377493/s | 155169 | 2.43x |   272108 | 110364 | 2.47x |
| store, 1000 per txn |         4K |   564114/s | 645829 | 0.87x |   310757 | 446428 | 0.70x |
| store, 1000 per txn |       128K |   607514/s | 660349 | 0.92x |   394968 | 443173 | 0.89x |
| point fetch         |         4K |   249703/s | 175871 | 1.42x |   228447 | 150565 | 1.52x |
| point fetch         |       128K |   251231/s | 159877 | 1.57x |   235153 | 117956 | 1.99x |
| full scan           |         4K |     5.75M/s | 10.36M | 0.56x |   5.36M | 9.11M | 0.59x |
| full scan           |       128K |     5.96M/s | 10.05M | 0.59x |   5.71M | 8.00M | 0.71x |

Reduced durability, zs_nosync 1s against WAL/NORMAL, rowid at 4K: 81419 vs
55751 at 1 per txn (1.46x), 419878 vs 285873 (1.47x), 661708 vs 613234
(1.08x), 739449 vs 1367336 (0.54x).  At 128K: 71081 vs 49715 (1.43x),
426240 vs 262934 (1.62x), 796654 vs 599486 (1.33x), 891902 vs 1361455
(0.66x).

Five readings.

**Small durable transactions are the win by a wider margin than any
previous run, and the whole of the change is one `fdatasync` per commit.**
4.5-5.1x ahead at 1 and 10 records per transaction where the 2.2.0 matrix
recorded 2.3-2.9x, 2.2-2.5x at 100, and at 1000-per-txn rowid is
**0.87x/0.92x** of the btree.  The attribution is unusually clean because
the durable 1-per-txn row was FLAT across every library between them
(5486/s on 2.2.0, 5507/s on 2.5.0) and then moved 86% on 2.6.0, which is
the release that removed the gate; the stock column is the control and
moved 1% (rowid 1000-per-txn 614757 -> 645829 at 4K).

**The target, re-derived.** At 4K: raw 735540/s = 1.360us/record, SQL
564114/s = 1.773us, so this engine adds **0.413us/record**; stock's
645829/s is 1.548us, so raw would need 1.135us = **~881k stores/s** for
SQL-on-zeroskip to match the btree there.  At 128K: overhead 0.504us, and
raw would need **~989k**.  Raw sits at 736k and 875k, so it is 84-88% of
the way.  The additive assumption keeps holding: our overhead has measured
0.41-0.50us/record across three libraries and a 1.2-1.5x change underneath
it, which is what licenses subtracting it.

**recordsize is the biggest lever available, bigger than anything the
library exposes, and the previous matrix was wrong to say it did not
matter.** 128K beats 4K by 8% on the rowid 1000-per-txn row (564114 ->
607514) and by 27% WITHOUT ROWID -- and at 2M records, where the repack
cascade actually runs, by **37%** (270964 -> 370217).  The mechanism is in
the cascade's own timings: rewriting the same 830MB takes 3696ms at 4K and
1939ms at 128K, and the conversions 735ms against 365ms.  A cascade does
large sequential rewrites, which is exactly what a big record favours.  The
old "4K and 128K agree within 1-5%" reading came from a database small
enough that the cascade never ran.  Combining it with the two library knobs
(`zs_rollover=16MB` and a deferred cascade) reaches -31% against the 4K
default; see below for why those two multiply.

**A fifth reading, and it is the one that flipped sign: at matched
durability we now beat WAL.**  `zs_nosync` against WAL/`synchronous=NORMAL`
is 1.43-1.46x ahead at one record per transaction where the 2.2.0 matrix had
it 0.93-0.95x BEHIND, and 1.47-1.62x at ten.  None of that is the gate --
`zs_nosync` issues no `fdatasync` at all, and the row is flat across it
(79721 -> 81419 at 4K).  It is 2.2.1's fold-in-place fix finally showing up
on this box, exactly where the doc predicted it would: that row is the only
one in either matrix that isolates per-commit CPU.  We still lose the
1000-per-txn row 0.54x/0.66x, so the WAL comparison has the same shape as
the journal one -- ours wins where commits are small.

**SQL-on-zeroskip is 69-77% of raw on rowid tables and 41-45% WITHOUT
ROWID**, the latter for the structural reason documented below (an index
tree's key is the whole record, so the row is written twice).  Per record,
our layer costs 0.41-0.50us on a rowid table and 1.39-1.88us WITHOUT
ROWID.

#### What the seek work bought, paired

The same box, dataset, `n`, reps and library, differing only in the
engine commit: the "before" arm predates the meta cache and
both `sqlite3BtreeTableMoveto` changes.  Rowid shape, durable:

| workload            |   before |    after | change | stock control |
|---------------------|---------:|---------:|-------:|--------------:|
| store, 1 per txn    |   5330/s |   5279/s |    -1% |  2011 -> 1953 |
| store, 10 per txn   |  45631/s |  47638/s |    +4% | 17986 -> 17215 |
| store, 100 per txn  | 170117/s | 239045/s |   +41% | 146648 -> 150797 |
| store, 1000 per txn | 252058/s | 446859/s |   +77% | 620367 -> 649097 |
| point fetch         | 256000/s | 247359/s |    -3% | 177123 -> 173649 |

The stock column is the control and it moves by -4% to +5% across the
pair, which is this box's run-to-run spread; the two zeroskip store rows
that moved are 8x and 15x that.  `WITHOUT ROWID` gained +8% at
100-per-txn and +11% at 1000 and was flat elsewhere, which is the meta
cache alone -- the append-bound work is rowid-only, because an index
tree's shortcut proves absence rather than positioning.  zs_nosync,
rowid: +60%/+94%/+101% at 10/100/1000 per txn.

**Small durable transactions are the win**: 2.7-2.8x at 1 and 10 per
transaction.  The mechanism is visible in the stock column -- switching
stock from journal to WAL takes 1-per-txn from 1869 to 41113/s, a 22x
swing, so nearly all of the btree's weakness here is the journal file
create/fsync/delete per commit rather than the fsync itself.  Per-commit
latency is 535us for stock and 189us for zeroskip on P4510s, which is
not media latency: it is a ZIL write to a raidz2 vdev with no SLOG, plus
zstd, plus AES.

**The crossover is still between 100 and 1000 records per transaction,
but it moved.** Below it zeroskip wins by 1.6x at 100-per-txn (it was
1.14x for rowid); above it the btree wins by 1.45x (rowid) to 1.59x
(WITHOUT ROWID), where it used to win by 2.5x and 1.8x.

**The layer is now nearly transparent on the rowid path, and the
remaining overhead is the index-key path.** At 1000-per-txn on this run
raw `zsbench` stores 567669/s; SQL-on-zeroskip reaches 446859 with a
rowid table (**79% of raw**) and 279533 WITHOUT ROWID (**49%**).  At
100-per-txn it is 87% and 62% of raw.  At 1 per txn it is 99% -- that
row is entirely fsync.  So the 1.45x the btree keeps at 1000-per-txn on
rowid tables is mostly the library's per-transaction cost and belongs
upstream.  The ratio also improves with N rather than being a
small-N artifact: on the laptop, rowid durable stores are 85% of raw at
20k records and 92% at 200k.  (`zsbench` ran once per shape, so it is
also a repeatability check on itself: the two passes agree within 0.6% at
1000-per-txn and within 2-5% on the durability-bound rows, the same
spread the SQL columns show.)

#### The WITHOUT ROWID gap is bytes, and it is structural

WITHOUT ROWID reaching only 49% of raw where rowid reaches 79% looks like
an index-key inefficiency to go fix.  It is not.  For an index tree the
key is the comparable encoding of the WHOLE record and the value is that
same record again, so a WITHOUT ROWID row appends roughly twice the bytes
a rowid row does -- for the benchmark's row, ~118 bytes of key plus ~110
of value against 12 plus 105.

Measured rather than assumed: shrink the value from 100 bytes to 8 and
WITHOUT ROWID gains +67% on 1000-per-txn stores (881080 -> 1473200/s,
laptop, zs_nosync) while rowid gains only +17% (1559357 -> 1820203).  The
shape gap closes from 1.77x to 1.24x.  Most of the penalty is
proportional to the value size, which is what duplication predicts and
what an encoding or probe inefficiency would not.

Neither obvious fix is available:

- **Shorten the key** to `nKeyField` fields.  Correct only when the
  leading fields are already unique -- true for a WITHOUT ROWID PK index,
  false for a non-unique index on a rowid table, where the trailing rowid
  is the discriminator.  `KeyInfo` carries `nKeyField` and `nAllField`
  and nothing that separates the two cases (both have
  `nAllField == nKeyField+1` for a two-column table), so the engine
  cannot tell without information from above `btree.h` -- which is the
  one thing this port does not spend.
- **Drop the value** and rebuild the record from the key on read.  The
  encoding is deliberately LOSSY -- NOCASE and RTRIM keys are collation
  transforms -- so for those indexes the record is not recoverable at
  all, and for the rest it trades write bytes against every read, on the
  side of the ledger where we are already ahead of stock by 1.4x.

So the duplication is what pays for "reads never decode", and it is
priced here rather than treated as a bug to chase.

#### The library 2.1.2 bump, paired (2026-08-17)

Upstream took the store row and rewrote the commit fold: it merges a
transaction's span as a SORTED RUN instead of inserting each record, and a
store walks the pending set once instead of twice.  The old fold searched
a structure that grew with the transaction, so a bigger transaction was
slower per record -- which is the opposite of what a batching caller
expects, and no benchmark that commits at one size can see it.

Interleaved A/B on this laptop, two libraries built from the same tree,
three passes each, medians (the interleaving matters: one arm alone swung
14% between runs):

| records/txn        | f363402   | 6060490   | ratio |
|--------------------|----------:|----------:|------:|
| 100                |   862634  |   875423  | 1.01x |
| 1000               |  1710718  |  2474326  | 1.45x |
| 10000              |  1760426  |  2915495  | 1.66x |
| 100000             |  1496803  |  4008739  | 2.68x |
| all 200k in one    |  1021780  |  3986773  | 3.90x |

The curve used to turn DOWN past 10000 per transaction and now keeps
climbing, which is the part that matters for us: SQLite's own bulk paths
are single-transaction.

What this engine inherits, same interleaving, 20k records: rowid
1000-per-txn **+32%** (1672403 -> 2201157), WITHOUT ROWID +20% at 1000
and +18% at 100, and 1-per-txn flat -- as it must be, since the library's
own 100-per-txn row did not move either.  A 200k-row bulk load in ONE
transaction through the SQL shell drops user CPU from 0.33s to 0.18s
(**1.83x**) on a rowid table and 0.47-0.67s to 0.22s (~2.4x) WITHOUT
ROWID; wall clock moves only ~1.2x here because `sys` is flat at ~0.4s
and this laptop's bulk load is I/O-bound, so expect more of it on a box
where the CPU is the constraint.

Our per-record overhead is unchanged by their work, which is what the
additive model behind the target below assumes and now verifies: at 20k
records the gap between raw and SQL is 0.122us/record on the old library
and 0.111us on the new, across a 1.38x change underneath it.

#### What belongs upstream, and the target after the bump

At 1000-per-txn the raw library was itself below the stock btree on
production (567669 vs 649097/s), so that row could not be won in this
engine.  It was also not per-commit cost, which is the natural guess: the
library's own sweep went 1000-per-txn 567669 -> 10000-per-txn 622337 ->
all-in-one-txn 602029, so collapsing 20 commits into 1 moved it under
10%.  The cost was per RECORD, and 2.1.2 is the answer to it.

**The ~939k stores/s target this document used to carry is VOID until it
is re-derived on the EPYC, and upstream is right about why.** It came from
stock's 649097/s (1.541us/record) less this engine's 0.476us/record, both
measured under the OLD library, and the subtraction assumes our share stays
additive while the library speeds up.  The laptop says it roughly does
(0.122us/record before the bump, 0.111us after, across a 1.38x change
underneath) -- but raw moved 1.4-3.9x depending on batch size, so our share
is now a far larger fraction of the row and the arithmetic deserves
re-measuring rather than re-using.  Nothing should be spent against that
number until a production run supplies both halves again, and that run is
also the one that should carry the 128K recordsize arm.

`sample` over raw `zsbench`'s own store row, no SQL in the picture
(laptop, 2M records, 5 reps -- a bigger database than the matrix's 20k,
where repacking barely runs, so read this as the at-scale regime and not
as an explanation of the matrix).  Both libraries, since 2.1.2 rewrote
this path; leaf samples, and the run rate rose 1.25M -> 1.60M stores/s:

                            f363402   6060490 (2.1.2)
    write                      1604      1664
    fdatasync                   742       692
    zsi_rec_decode              617       755
    zsi_index_fold_run            -       504
    memcmp                      692       483
    XXH3                        689       415
    zsi_pend_lb                 392       166
    stat/fcntl/fstat/open/unlink 1536     1555
    zsi_repack_run              185       188

The two changes upstream made are visible and only those: `zsi_pend_lb`
halves and `memcmp` falls by a third, which is one walk of the pending set
per store instead of two, and the per-record `zsi_index_insert` is gone,
replaced by `zsi_index_fold_run` merging the span.

What did NOT move is worth handing back.  **Record decode is now the top
CPU symbol** (755 samples, ~11% of the sampled total) -- it is what the
fold's merge and the repacker both spend their time on, and upstream's note
that `zsi_index_fold_run` is "under 2% of a store profile" does not match
7% here, so the conditions differ and are worth reconciling rather than
assuming one of the two is wrong.  And **the filesystem metadata calls are
unchanged at ~22% of sampled time** (1555 vs 1536) even though the
per-call-site counts refuted the mechanism I guessed at.  Refuting the
explanation did not remove the cost.

Two things worth upstream's attention.  The metadata syscalls are ~15-18%
of the row and they are NOT per-commit: at 10000-per-txn, with a tenth
the commits, `fdatasync` drops 742 -> 432 as expected while `stat` only
goes 454 -> 385, `fcntl` 384 -> 345 and `open` 242 -> 204.  They track
file lifecycle rather than transactions.  And `zsi_rec_decode` at 617
samples in a workload that only ever stores suggested the repacker
reading records back mid-load.

**Both readings were wrong, and upstream's exact counts say how.**  Kept
here because the mistake is instructive and the same table would produce
it again.

The metadata calls ARE per-commit -- about 12 of them, counted by macro
shims over every syscall: 1 `stat` (the C-4i freshen probe), 2 `fcntl`
(C-1e locking), 2 `fstat`, 1 `mmap` + 1 `munmap` (the terminator
checksum's `zsi_txn_at`), 2 `fdatasync` (C-7; **one** from library
2.6.0).  My "not per-commit"
inference came from watching the WRONG SUBSET: at a tenth the commits
`fdatasync` fell 5.3x, `fcntl` 5.0x, `fstat` 5.6x and `stat` 7.1x -- all
per-commit as expected -- while `open`/`close`, `readdir`, `rename` and
`unlink` stayed flat at ~1.17x.  That flat group is FILE LIFECYCLE, the
cascade and the conversions, and it is the group that hurts on ZFS,
because those calls resolve paths and dirty directories.  A `sample` leaf
table shows the sum of the two and neither trend.

Five of the twelve cannot go (2 `fdatasync`, 2 `fcntl`, the C-4i `stat`) --
four, since 2.6.0 made it one `fdatasync`, and that one turned out to be the
single most valuable call in the list (+86% on production; see below).
Three might, and upstream has not done them: `zsi_file_remap`'s `fstat`
re-reads a size the writer just produced, `zsi_txn_stream_begin`'s `fstat`
duplicates the freshen probe but wants the fd's real end (F-24/R-4
territory after a crash), and the per-transaction `mmap`/`munmap` could
share the handle's headroom mapping.  All three are worth ~nothing on
APFS, so **whether they matter is a question only the EPYC/ZFS profile can
answer, and answering it is on us.**

The decode share was BOTH of us wrong, in opposite directions: upstream's
"under 2%" was `zsi_index_fold_run`'s SELF time, which excludes the decodes
it drives; my 7% read the whole `zsi_rec_decode` row as the fold's, which
credits it with decodes belonging to three other callers.  Attributed by
nearest owning frame, `zsi_rec_decode` is 37.6% fold, 25.9% convert, 20.1%
unordered replay, 16.4% repack; inclusively the fold is **3.8%** and the
repacker is **40.3%**, climbing to 48.9% at 20M records.  So the "different
conditions" in my report were the cascade, not the fold.

**The lesson, and it is ours as much as theirs: a flat top-of-stack table
answers "which symbol burned time", never "which subsystem did."**
`zsi_rec_decode`, `memcmp` and `memset` each have several callers -- `memset`
turns out to be 99.7% the repacker, which is why deleting a memset on the
store path measured 0.0%.  Every flat-table reading in this document is
subject to that, including the historical one below.

#### What the syscalls cost on ZFS, which is the answer upstream wanted

`strace -c -f` over one 2M-record bulk store at 1000-per-txn, both engines,
same dataset (4K, zstd, raidz2, no SLOG).  Totals first, because they are
the surprise: **zeroskip spends 2.90s in syscalls against the btree's
0.70s -- 4.1x -- while making 4x FEWER calls** (36,413 against 147,846).
Its syscalls are individually far more expensive.

| zeroskip           | share  | calls | us/call | | stock         | share  | calls | us/call |
|--------------------|-------:|------:|--------:|-|---------------|-------:|------:|--------:|
| fdatasync          | 41.5%  |  4552 |     264 | | pwrite64      | 36.4%  | 89239 |       2 |
| write              | 23.3%  |  6594 |     102 | | fdatasync     | 23.1%  |  8004 |      20 |
| unlink             | 17.2%  |   270 |    1849 | | unlink(at)    | 14.8%  |  2003 |      25 |
| close              |  5.0%  |  2139 |      67 | | openat        |  3.8%  |  4026 |       6 |
| munmap             |  1.9%  |  2730 |      19 | | fcntl         |  2.1%  | 18013 |       0 |
| openat/fstat/mmap  |  0.9%  | 12287 |     0-5 | | newfstatat    |  1.7%  | 12054 |       0 |
| getdents64/fcntl   |  0.3%  |  6114 |     0-3 | |               |        |       |         |
| rename             |  0.1%  |   157 |      25 | |               |        |       |         |

**`fdatasync` costs 264us of SYSTEM time here against the btree's 20us**, a
13x difference per call -- and the units matter: at this span size system time
is ~74% of wall (2026-08-19), so this is mostly a CPU figure, which is what
the profile below turns out to say.  21.8% of all cycles are
`fdatasync` -> `zpl_fsync` -> `zil_commit_impl` -> `zil_lwb_write_issue`
-> `zfs_get_data` -> `dmu_sync` -> `arc_write` -> `zio_write`.  That is a
ZIL write of a large append to a raidz2 vdev with no SLOG, which is
inherently more work than syncing a 4KB journal page.  46.8% of cycles are
inside syscalls altogether.  The 2026-08-19 run adds stock's half of the
picture, which had never been profiled: stock's 46.0% of cycles under
`do_syscall_64` is `__x64_sys_pwrite64` -> `vfs_write` -> `zfs_write` ->
`dmu_assign_arcbuf_by_dnode`, i.e. buffered, with the compression and
encryption deferred to a txg.  **The reason for the 13x was got wrong here
three times and a call graph settled it on 2026-08-19: it is neither a ZIL mode
difference nor inline crypto.** See "What our fdatasync actually spends its CPU
on" below -- there is no compression, encryption or checksum in the caller's
stack at all, and what is there is per-BLOCK setup.

**On the three calls upstream offered to remove: not worth it, and the
reason corrects a mistake this section first made.** `newfstatat` is 0.28%
and `mmap`+`munmap` together 2.1% of syscall time, so all three would save
at most ~2.4% of 2.90s.  The tempting extrapolation -- that the mapping
pair is per COMMIT, so 20000 single commits would pay 20000 of them and
the fix would be worth ~0.4s there -- is wrong.  `zsi_txn_at` maps only
when the span has spilled out of the append buffer (64KB then, up to 4MB
since 2.6.0); a transaction
small enough takes the `in_chunk` branch and maps nothing.  Upstream's own
per-call-site counts show exactly that shape: 1.35 mmap+munmap per commit
at 1000-per-txn but 3.99 at 10000-per-txn, i.e. rising with transaction
SIZE, not with commit count.  So the mapping change helps large
transactions, where it is worth ~2%, and does nothing for the
single-commit row.

**And `unlink` is the file-lifecycle cost, not `open` or `readdir`.** 270
unlinks take 0.50s -- 17% of syscall time -- at **1.8ms each**, against
25us for the btree's.  They are large repacked-away generations, so on ZFS
each one frees many blocks through zstd and raidz2 parity.  That is the
group upstream asked about, and it points at repack policy rather than at
path resolution: fewer, larger merges unlink less often.  It is also the
concrete argument for bounding rather than tiering.

#### The syscall picture INVERTS with batch size, and that is the matrix

`strace -c -f` over the same load at both ends of the transaction-size range,
2M records at 1000-per-txn and 20k at one per txn, durable, on ZFS.  The two
engines swap places, and the reason is one sentence: **zeroskip's per-COMMIT
cost is small and nearly fixed, and its per-BYTE sync cost is large; the btree
is the other way round.**

| | zeroskip | stock btree |
|---|---:|---:|
| 1 per txn, total syscall time | **0.91s** / 182k calls | 4.20s / 781k calls |
| 1000 per txn, total syscall time | 3.09s / 36k calls | **0.79s** / 148k calls |

Per commit at one record per transaction, zeroskip issues 2 `fdatasync`, 2
`write`, 3 `newfstatat` and 2 `fcntl` -- nine calls, and 41 unlinks in the
whole run.  Stock issues 4 `fdatasync`, 10 `pwrite64`, 2 `openat`, 9 `fcntl`,
6 `newfstatat`, 1 `fchown` and **1 unlink**, the last costing it 0.72s across
20000 commits, or 17% of its syscall time, because that is the rollback
journal being created and deleted per transaction.  That is the mechanism
behind the 2.3-2.9x this engine won at 1 and 10 records per transaction when
this was measured, 4.5-5.1x since library 2.6.0 halved the `fdatasync` count,
and it is not subtle once counted.

#### The per-byte sync deficit does not exist, and system time invented it

This section used to end: our advantage at small transactions is per-commit
syscall count, our deficit at large ones is per-BYTE sync cost, and the
matrix's crossover between 100 and 1000 records per transaction is where the
two curves meet.  **The second half is wrong.**  It came from a sweep in system
time, and once `strace -c -w` measured wall time (2026-08-19) the deficit
disappeared -- there is no crossover, because there is no second curve.

    fdatasync, WALL time per record, straced both sides identically
    (absolutes are inflated by ptrace; the ratio is the measurement)

    records/txn      zs calls/commit   stock   zs us/rec   stock us/rec   ratio
    1                     1.00          4.00      125.6       496.0       3.95x
    10                    1.00          4.00       11.8        52.0       4.42x
    100                   1.03          4.00        1.8         5.9       3.25x
    200                   1.06          4.00        1.3         3.4       2.69x
    400                   1.12          4.01        1.1         1.9       1.76x
    1000                  1.27          4.02        0.8         1.0       1.30x

**We are cheaper on `fdatasync` at every transaction size measured, and the
advantage narrows without ever inverting.**  Per call the two engines are
indistinguishable at the small end -- 125us against 124us -- because that is
the pool's ZIL commit latency and it is nearly fixed.  What differs is how many
times each engine pays it: **stock pays it four times per commit and we pay it
once.**  Per byte the slopes are 3.74us/KB for us against 2.82 for stock, a
factor of 1.33 -- not the "9x per synced byte" this document reported for three
rounds.

Why system time said otherwise: our `fdatasync` burns CPU in the committing
thread that stock's does not -- about half its wall time (312us system of
611us) against stock's 6%.  That asymmetry is measured and stands.  **What it
is made of took three wrong answers, and it is not compression and
encryption, which is what this document claimed here until the call graph
arrived.**

#### What our fdatasync actually spends its CPU on

Not crypto.  A call graph of the same 2M-record load (`perf record -g`, process
only, 4K dataset) puts `fdatasync` at 8.59% of cycles and names 73% of it:

    zio_create (kmem alloc + memset_orig)           2.06%   24% of fdatasync
    taskq_dispatch_ent -> __wake_up -> try_to_wake  1.70%   20%
    zfs_zget                                        1.06%   12%
    zfs_rangelock_enter_impl                        0.86%   10%
    dmu_buf_hold_noread                             0.62%    7%

Allocate a zio and zero it, take a rangelock, look up the znode, hand off to a
taskq and wake it.  `zio_compress_select` is 0.03% of the whole profile and
`zio_checksum_select` 0.01%; zstd, aes and gcm do not appear at all.

`zio_nowait -> zio_issue_async -> taskq_dispatch_ent` at 1.70% is the handoff
itself, which is the direct evidence: `ZIO_STAGE_ISSUE_ASYNC` precedes
`ZIO_STAGE_WRITE_COMPRESS` in the write pipeline, so the caller dispatches and
never runs the compress or encrypt stage.  Both engines' crypto is on a
`z_wr_iss` taskq thread.  ZFS spawns nothing per write, either: the ZIO taskqs
and the per-pool `txg_sync_thread` are created at pool import and parked.

**That predicted something the sweep had not tested, and the answer is
"directionally yes, nowhere near the predicted size".** If the cost were
per-BLOCK setup, a 130KB span is ~32 blocks at `recordsize=4K` against one or
two at 128K, so the slope should mostly flatten.  Measured on both datasets
(2026-08-20, wall):

    per-txn      1     10    100    200    400   1000     slope
    4K        115us  111us  179us  236us  391us  647us   4.09us/KB
    128K      108us  115us  153us  203us  404us  485us   2.90us/KB

**29% shallower, not flat.** A 32-fold reduction in block count buys under a
third off the slope, so per-block setup is a real term and not the dominant
one; something else in that path scales with bytes.  This is the fourth
mechanism offered for this row and the first that was tested before being
believed, which is the only reason it can be reported as a partial hit rather
than a discovery.

Caveat on the profile, in both directions: `perf record` on the process sampled
only `zskvbench` (98.13% of cycles), so it proves the crypto is not in OUR
thread and says nothing about what the taskqs cost.  `perf record -a` is what
would price those.

**So the matrix crossover has a different cause, and it is one we can act on.**
At 1000 records per transaction we lose the store row to the btree while being
1.3x CHEAPER on sync, which leaves exactly one candidate: the repack cascade,
which rewrites 4.9x the stored bytes at the default rollover.  That is now
confirmed from the other direction -- raising `rollover_size` to 64MB cuts the
rewriting to 1.9x and takes the row from 7.44s to 5.16s at 4K, +44%, which no
change to the sync path could have done.  The thing to attack for large
transactions is the rewriting; the sync path was never the problem.

The practical consequence for a caller is unchanged in direction and stronger
in size: this engine's edge is per-commit cost, it is largest where commits are
smallest (4.4x on sync at ten records per transaction), and a mail server that
commits per message is on the right side of it.

#### A per-commit regression, found here and fixed upstream

Between the 2026-08-17 and 2026-08-18 runs the `zs_nosync` 1-per-txn row
fell **32%** (rowid 4K 75625 -> 51505; 128K 74053 -> 47801) while every
durable row and both stock columns stayed flat.  What it took to hand it
over usefully:

- not this engine.  Both changes since were in the savepoint undo path, and
  instrumenting the funnel showed `zskvbench` opens no undo mark at all;
- not file churn.  Polling the directory through a 20000-single-commit run
  counted ~130 files created either way, old library and new;
- the library, reproduced at ~5% on this laptop by standalone `zsbench`
  binaries built from f363402 and 6f58076 sources with the engine
  uninvolved, five interleaved passes, new slower in every one.

Upstream root-caused it to 2.1.2's own sorted-run fold (`4451a32`): the
merge went FORWARDS into a fresh allocation, so each commit copied the
whole delta and did a malloc/free -- O(delta) per commit, where the
per-record insert it replaced did a bounded memmove and no allocation.  For
a run of one record that is a straight loss.  It now merges in place and
backwards.  Confirmed here on 2.2.1, interleaved, 1-per-txn under
zs_nosync: **111418/s -> 153932/s, +38%**, with 10-per-txn +21%, 100-per-txn
+6% and 1000-per-txn flat.

**My model of it was wrong in a way worth keeping.** I attributed the gap
to ZFS multiplying a small cost about sixfold.  It needs no filesystem
amplification at all: it is ~2us/record of CPU, and this box's cores are
roughly 3x slower than the laptop's, which is the whole of the 6us/record
seen on production.  The reason every durable row stayed flat is that 2us
disappears under a 264us `fdatasync` -- so **the reduced-durability
1-per-txn row is the only row in either matrix that isolates per-commit
CPU**, and it is the row that caught a regression while everything else got
faster.  That is the argument for keeping a row that looks redundant.

#### A bulk load writes several times what it stores

Library 2.2.0's `zs_db_stats` (A-17) counts what a handle has REWRITTEN
since it was opened, repacks and conversions apart, with records, bytes and
nanoseconds for each.  `sqlite3ZsStats` in `btree_zs.h` exposes it, and
`zskvbench` now prints it after its 1000-per-txn row, so every run says how
much of its write cost went on rewriting -- which is invisible from SQL and
is a large part of what separates this engine's bulk-store row from the
btree's:

    store, 1000 per txn                1844846/s   0.11s
    rewritten             2.7x of stored  (11 conversions 25MB 13ms;
                                           4 repacks 36MB 18ms; stored 22MB)

200k records, 100-byte values, laptop.  Upstream measures ~5x at 2M records
(117 conversions, 250MB, 142ms; 39 merges, 796MB, 521ms), so the multiple
grows with the database, which matches the repacker's share of their store
profile climbing from 40.3% at 2M to 48.9% at 20M.

Two things about the counters.  They are PER HANDLE, so another process's
repacks are invisible and always will be -- nothing on disk records who
rewrote what.  And `zs_db_compact` counts in the repack half deliberately (a
compaction IS a repack of everything), so a VACUUM's cost lands there;
bracket the call with two reads to separate it.  Conversions are counted
apart because they are structural -- every generation converts exactly once,
and no setting moves that -- so a combined figure would invite tuning the
half that cannot move.

`stored` here is key+value bytes and the library's byte counts include its
record framing, so the multiple is comparable across our own runs rather
than against upstream's figure.  The MB and ms are exact.

#### Deferring the cascade removes most of it (zs_norepack)

The repack cascade runs from a write transaction, and on a bulk load it is
most of the wall time (58% at 4K, above).  The obvious question was whether
it could be moved out of the write path into a maintenance window; the
answer turned out to be better than that -- **deferring does not move the
cost, it removes about three quarters of it**, because the ladder re-merges
the same bytes roughly three times on the way up and one merge at the end
does not.  Upstream measured it, we reproduced it, and the numbers agree:

    2M records, 1000-per-txn, laptop, rowid

    armed (default)      load 1.39s                        40 merges, 830MB
    deferred + catch-up  load 0.87s + catch-up 0.65s        1 merge,  263MB
                         = 1.52s total

The load itself is **60% faster** and the merge output drops **68%**, from
4.9x of stored bytes to 2.4x.  Conversions are identical in both (117, 263MB)
and structural -- every generation converts exactly once and no setting
changes that -- so the addressable part is the merging, and deferring
addresses nearly all of it.  Upstream's run also ends with FEWER files than
the armed one (2 against 5), because the cascade stops mid-ladder while a
catch-up runs to completion.

`zs_norepack=1` on the URI opens with `ZS_NOAUTOREPACK`, and
`sqlite3ZsRepackCatchUp(db, zDb, nMaxMerges, &nDone, &bBehind)` (btree_zs.h)
drives `zs_db_repack`, bounded to `nMaxMerges` or unbounded at 0.  It is
bounded because the intended caller is a post-response delayed-work slot rather
than a benchmark: one merge rewrites a whole generation and cannot be
interrupted, so "catch up completely" is the wrong contract for anything with a
latency budget, and `bBehind` is what lets such a caller tell "done" from "gave
up early" and reschedule itself.  `zskvbench --defer-repack` composes it with
the URI parameter and times the two apart.

**This is a bulk-load-window shape, not a mode.** During the window the file
count is unbounded -- upstream measured 118 files at 2M records with the
cascade off and never caught up, and point-lookup cost is linear in the file
count (D-14d), so a database left like that reads badly and `readdir` alone
goes from 6,378 calls to 29,723.  It suits a restore, a migration or an
initial import, with the catch-up run before readers come back.  It is not
a default and this engine does not set it.

Measured on ZFS (2M records, 1000-per-txn, rowid), deferring is worth
**-15% of total wall at 4K** (7.61s armed against 6.46s deferred) and
**nothing at all at 128K** (5.62s against 5.69s).  I predicted 34-38% from
the byte saving and that was too high, for a reason neither side had
modelled: **a merge's cost per byte is set by its INPUT FILE COUNT, not just
by its bytes.**  The deferred run's single merge reads the 117 generations
the load left behind and manages 9.5ms/MB, where the cascade's 40 merges
average 4.4ms/MB -- so a third of the bytes buys only a third off the merge
TIME, and at 128K, where bytes are cheap, it buys nothing.

That also says how to cash the byte saving, and it is the combination in the
next section: with `zs_rollover=16MB` the deferred merge reads 15 large
generations instead of 117 small ones and runs at 2.6ms/MB -- the cascade's
own rate -- so the 68% byte saving finally shows up in the clock.

Every catch-up in `zskvbench` is followed by an untimed
`PRAGMA integrity_check`, because this is the only place in the tier that
drives a merge from our side rather than the library's.

#### The one measurement a resident fixture cannot make

Library 2.9.0 added a `posix_madvise(POSIX_MADV_WILLNEED)` per merge input, on
the strength of our own call graph: 11.6% of a bulk load was page faults under
`XXH3_hashLong`, in the verify pass that opens a conversion or a repack.
Upstream cannot measure it -- on APFS the input pages are resident from having
just been written -- and their position is that if it does not show on ZFS it
has no justification left and should come out.

**Every fixture in this tree is the worst possible case for observing it**, and
noticing that is most of the work.  They all write the merge input and merge it
moments later, so the pages are hot; and on the production box a 260MB database
fits in ARC many times over, so "run it on ZFS" does not fix the fixture.  An
A/B over the existing cascade phase would measure nothing and would wrongly
condemn the change.

`test/zs/prefetch-ab.sh` is the measurement that can see it.  The phases run in
separate processes with a cache drop between them, which is what
`zskvbench --build-only` and `--catchup-only` exist for:

    zskvbench --build-only     leave a database, cascade disarmed, files unmerged
    echo 3 > drop_caches       page cache cold
    zskvbench --catchup-only   time the merge alone, and count its faults

**Faults are the primary reading and the clock is secondary.** The hint acts on
faults; a fault count from `getrusage` is nearly noise-free where a wall clock
on a shared machine is not, and this project has already read machine noise as
a result more than once.  Both arms come from git rather than from editing the
source, so they differ by exactly one upstream commit, and `cmp` guards against
the byte-identical pair that once read as "no effect".  Arm order alternates,
because a fixed order manufactured a clean-looking 2.4% here once already.

**Calibrated on APFS, where it correctly reports nothing:** 14349 minor faults
and zero major on both arms, four alternating runs, times identical to 0.01s.
An instrument that reports no effect where no effect is possible is the
precondition for believing it when it reports one.

Two honest limits to state with any result.  `drop_caches` empties the page
cache and not the ARC, so a fault afterwards still finds its data in ARC rather
than on disk -- cheaper than a real cold read, so this **understates** the hint.
Making it colder means exporting and re-importing the pool or a database larger
than ARC, neither of which belongs in a script pointed at a production box.  So
if the effect shows, it is real; if it does not, that is evidence and not proof,
and the next step is a dataset bigger than ARC rather than deleting the call.

#### Commit latency as a distribution, and what a background repack buys

Every store number above is a rate, and a rate cannot answer the question the
deployment has.  Cyrus commits one message at a time, and the repack cascade
runs synchronously inside whichever write transaction trips it -- so one
delivery in N pays for a whole generation merge while the rest pay nothing.
That is a tail problem, and it is the actual argument for moving repacks off
the critical path: not throughput, which deferral no longer wins at all.

`zskvbench --latency N` does N single-record commits, times each one, and
reports percentiles.  It also ATTRIBUTES the outliers instead of assuming: the
library's repack and conversion counters (A-17) are sampled either side of
every commit, so a commit that merged is known to have merged rather than
inferred to have.  Without that the tail could equally be a txg boundary or the
append buffer growing, and the fix for each is different.

    20000 single-record commits, rowid, laptop, one pass

                        p50     p90     p99   p99.9     max
    cascade armed     0.026   0.037   0.093   0.440   2.184 ms
      merged: 25 of 20000 (0.12%), p50 0.476, max 2.184 -- 3% of total time
    zs_norepack=1     0.023   0.031   0.047   0.236   0.787 ms
      merged: 19 of 20000 (0.10%), p50 0.449, max 0.787 -- 2% of total time

**The tail is real, and disarming the cascade only half fixes it.** p99.9 is
17x the median.  `zs_norepack=1` takes p99.9 from 0.386 to 0.232ms -- but 19 of
the 25 slow commits survive it, and the counters say why: 19 conversions, 0
repacks.  A conversion is how a generation gets sealed into sorted form, it
happens once per generation, and `ZS_NOAUTOREPACK` says nothing about it.

**The lever for the other half is `zs_db_seal`, which this project had missed
and library 2.9.0 documents as the conversion latency lever.** A conversion is
unavoidable, but *when* it happens is not: by default it lands on whichever
commit grows the file past the generation bound, and calling seal from an idle
moment means that commit has nothing to do.  Exposed here as `sqlite3ZsSeal()`.

    20000 single-record commits, rowid, laptop, one pass

                            p50   p99.9     merging commits
    cascade armed         0.023   0.386     25 (0.12%), 3% of total time
    zs_norepack=1         0.023   0.232     19 (0.10%), 1%
    + seal every 500      0.024   0.226     NONE
    + seal every 4000     0.023   0.225     15 (0.07%), 2%

**Sealing every 500 commits removes the merging-commit class entirely** rather
than shrinking it: no commit in 20000 did any file-lifecycle work, and the 40
conversions all happened in the untimed seal.  p99.9 improves 42% against the
default.

**The cadence matters and 4000 is too slow, for a reason worth knowing: at one
record per transaction it is D-9d's SPAN bound that seals a generation, not
`rollover_size`.** 20000 commits produce 19 conversions -- one per 1024 spans,
not one per 2MB.  So the rule is to seal more often than `rollover_txns`, and
a cadence longer than it leaves conversions on the write path.

**And do not seal frequently with the cascade armed.** Sealing every 500 with
repacks still on the write path takes total rewriting from 3.2x of stored to
**4.3x** (40 conversions and 14 repacks against 19 and 6), because more,
smaller generations mean more to merge.  The combination that works is all
three together: `zs_norepack=1`, seal from idle, and a bounded catch-up from
idle.

Two honest limits.  `max` is not a usable statistic here -- it swings 0.4 to
5.5ms across arms on an unquiesced laptop and is OS noise; `p99.9` is the one
that moves consistently.  And this is APFS, where `unlink` is free and it is
~1ms on the production pool.

#### The seal ladder is a net loss on ZFS, and the laptop could not see why

Run on production (2026-08-20, 200000 single-record commits, both recordsizes),
the ladder above inverts.  **Retracting the recommendation.**

    4K              throughput    p50     p99.9      max   merging commits
    armed              9820/s   0.096    0.862   122.745   260 (4% of time)
    norepack           5518/s   0.158    0.901    32.846   195 (1%)
    + seal/500         2615/s   0.331    1.171    31.473   NONE

    vs armed:       norepack  -44% tp, +65% p50, +5% p99.9
                    +seal     -73% tp, +245% p50, +36% p99.9

128K is the same shape (-74% throughput, +248% p50, +44% p99.9).  **Sealing
removes the merging-commit class and pays for it with almost everything else:**
a quarter of the throughput, three and a half times the median, and a p99.9 that
gets *worse* rather than better.  The one genuine win is the extreme max, 122ms
to 31ms -- and the armed arm's own non-merging max is 33.7ms, so that 122ms
outlier really is a merging commit and really does go away.

**Why the laptop said the opposite: file count.** `zs_norepack` stops the
merging, so files accumulate, and point-lookup cost is linear in the file count
(D-14d) -- every commit's `OP_NotExists` probe searches all of them.  Sealing
every 500 commits makes it worse by turning 195 generations into 400.  At 20000
records on APFS the file count stays small and `unlink` is free, so neither cost
appears; at 200000 records on a pool where `unlink` is ~1ms, both do.  The
fixture that produced the recommendation was too small to contain its own
refutation -- the same shape as the recordsize finding that opens this document.

**So the deployment answer for a per-message writer is the DEFAULT**: cascade
armed, no `zs_norepack`, no seal cadence.  The tail is real (0.13% of commits,
4% of total time, one 122ms outlier in 200000) but every lever that removes it
costs more than it saves.  `sqlite3ZsSeal()` and the bounded catch-up stay --
they are the right tools for a bulk import, where throughput during the window
is not what matters -- and the p99.9 argument for backgrounding a repack is
withdrawn until there is a lever that does not multiply the file count.

A measurement bug worth recording, because it produced exactly the result the
fixture was built to look for: the first version sealed inside the loop and
sampled the counters afterwards, charging the seal's conversion to whichever
commit preceded it.  That read as "40 merging commits with a p50 of 24us" --
merging commits as fast as ordinary ones, which is impossible and was believed
for several minutes.  Attribution now happens before the seal, with a
re-baseline after it.

So the delayed-work recipe, for a caller that commits per message and has no
threads: open with `zs_norepack=1`, and from the post-response slot call
`sqlite3ZsSeal()` at least once per `rollover_txns` commits and
`sqlite3ZsRepackCatchUp()` with a small bound.  That is two calls and no
thread.  If a thread is wanted anyway, library 2.9.0's header now says a handle
is NOT thread-safe and that a second thread with its OWN handle is supported,
excluding the first exactly as a second process would (C-1j, G-5).

`sqlite3ZsRepackCatchUp()` takes an `nMaxMerges` bound and returns whether more
work remains, so a caller with a latency budget can do one merge per idle slot
and reschedule rather than block for the whole backlog -- a merge rewrites a
generation and cannot be interrupted once started.  Never disarming is the
failure mode to design against: point-lookup cost is linear in the file count
(D-14d), and a 2M-record load left un-repacked leaves 119 files and takes
`readdir` from 6,378 calls to 29,723.  So a delayed-work scheduler should
trigger on state (`*pbBehind`, file count) and not on idleness alone, with a
floor where a commit pays inline if the backlog crosses a threshold.

#### rollover_size, the pointer table, and what actually wins (measured)

A larger generation cuts both per-file axes at once -- fewer files, so fewer
lifecycle events, and a shallower ladder, so fewer bytes re-merged.  Upstream
counted 2MB -> 16MB taking a 2M-record load from 269 unlinks and 796MB of
merging to 33 and 449MB, 45x fewer unlinks by 64MB.  Reproduced here through
SQL, merge output 830MB -> 645MB -> 208MB.

But the axes are not the whole cost, and on production the ranking is not the
one the byte counts predict.  Total wall for the same load, 2M records at
1000-per-txn, rowid, against the 4K default of 7.61s:

| configuration                    |     4K |   128K |
|----------------------------------|-------:|-------:|
| default (2MB, cascade armed)     |  7.44s |  5.72s |
| rollover 16MB, armed             |  6.93s |  5.56s |
| **rollover 64MB, armed**         | **5.16s** | **4.70s** |
| deferred + catch-up (2MB)        |  6.30s |  5.90s |
| rollover 16MB + deferred         |  5.44s |  4.93s |
| 2MB + pointer table              |  7.53s |  5.74s |
| 16MB + pointer table             |  6.94s |  5.57s |
| 64MB + pointer table             |  5.08s |  4.56s |
| 2MB, RANDOM arrival order        | 18.55s | 16.40s |
| 16MB, random                     | 18.85s | 17.04s |
| 64MB, random                     | 17.34s | 16.49s |

Re-run on 2026-08-19 with library 2.8.0; the 2.2.1/2.3.0 figures this table
used to carry are in the publish-threshold section above.  **The ranking is
new, and the entry this table had carried unexplained since the first sweep is
gone.**

**`rollover_size=64MB` is now the best setting on both recordsizes** -- 5.16s
at 4K against the default's 7.44s (+44%) and 4.70s against 5.72s at 128K
(+22%) -- and it beats the previous champion, 16MB plus a deferred cascade, on
both.  For four libraries this table said the opposite, that the largest
generation was the WORST setting while rewriting a quarter of the bytes.  That
was a quadratic in the library's private in-memory index (above), and with it
fixed the byte counts finally predict the clock: 1.9x of stored against 4.9x,
and the fastest configuration is the one that rewrites least.  **"Minimising
rewritten bytes is not the objective function" is retracted; it was a bug
wearing the shape of a principle.**

**The best combination is now 128K + 64MB armed at 4.70s, -37% against the 4K
default**, and the deferred cascade is no longer part of it: deferring buys
-15% at 4K and *costs* 3% at 128K on 2.8.0, and 64MB armed is better than
either.  Deferral remains useful for the case it was built for -- an import
where the file count during the window does not matter -- but it is no longer
the recommended shape for a bulk load.

**The pointer-table rows remain free**, within 1% of their table-less
counterparts at every generation size including 64MB, where the cost was once
+214%.  That is 2.5.0's "a commit publishes nothing" holding on ZFS.

**One qualifier that decides whether any of this reaches Cyrus.** D-9d bounds a
generation at 1024 spans as well as `rollover_size` bytes, and a per-message
writer trips the span bound first: 1024 single-record commits is about 133KB,
far below even the 2MB default.  So `rollover_size` is inert for per-message
steady state and the 64MB win is a BULK-LOAD win -- imports, restores,
migrations, the `sqlite3 .dump` path.  Set it there; it changes nothing in the
mail-delivery path, which is what the cliff table above already showed from the
open side.

Four things, in order of how much they matter.

**`rollover_size` is now the biggest single lever for a bulk load, ahead of
recordsize.** 64MB is worth -31% at 4K and -18% at 128K; recordsize is worth
-23% at the default rollover and -9% at 64MB, because the two overlap -- both
are paying for the same cascade rewriting, one by moving fewer bytes and one by
moving them faster.  **The best combination is 128K + 64MB armed at 4.70s,
-37% against the 4K default.**  The explicit `zs_rollover=2MB` run reproduces
the default to within 1%, which is the control on the parameter itself.

**There is no knee any more, in either direction.** The sweep is monotonic on
both recordsizes for the first time -- 7.44 > 6.93 > 5.16 at 4K, 5.72 > 5.56 >
4.70 at 128K -- so "measure it, do not assume bigger" now says bigger, up to
the largest size measured.  What that costs is untested above 64MB and the
reason to stop there is not performance: a generation is replayed at open, so
the ceiling is set by open latency and by how much a crash has to redo, neither
of which this sweep prices.

**The pointer table's cost with a large generation was a LIBRARY DEFAULT, not
a refutation of the idea.** The production figures above -- -9% at 2MB, -42%
at 16MB, -71% at 64MB -- were taken on 2.2.1, whose default publish threshold
was an absolute 32KB: a generation published about 2000 times whatever its
size, each publish rewriting a table proportional to the whole generation, so
the cost was quadratic in generation size (upstream measured 4.70GB of table
writes at a 64MB rollover).  Library 2.3.0 scales the threshold with the file
it describes, which bounds both ends from one rule.

Re-run on production, 2M records at 1000-per-txn, total wall with the table
against the same configuration without it:

| rollover | 4K on 2.2.1 | 4K on 2.3.0 | table's cost, 2.2.1 -> 2.3.0 |
|----------|------------:|------------:|-----------------------------:|
| 2MB      |       8.38s |       8.42s |          +10% -> **+9%**     |
| 16MB     |      12.62s |      11.10s |          +72% -> **+50%**    |
| 64MB     |      26.23s |      12.17s |         +214% -> **+44%**    |

At 128K the same shape: 64MB + table 17.84s -> 10.28s (+122% -> +26%), 16MB
9.57s -> 8.85s (+64% -> +49%).  So **the fix is decisive at 64MB (2.2x) and
marginal at 16MB (-12%)**, and the honest reading is that it bounded the
pathology rather than removing the cost: the table still costs +44-50% during
a load at a large generation against +9-16% at the 2MB default.  Publishing
during a bulk load is expensive whatever the threshold, which is the part of
the original conclusion that survives.

What survives is the trade rather than the refutation: the table's cost lands
during writes and its benefit at open, so one setting cannot balance them --
which is why the next section measures our open side.

#### One gate per commit (library 2.6.0), and a prediction that inverts

Upstream removed the sync between a span's records and its terminator: the
terminator's checksum already makes a terminator that reaches disk without its
data read as absent (F-22), so ordering the writes made that case impossible
rather than merely detectable.  One `fdatasync` per commit instead of two.

Interleaved here, 20k records, three passes, durable:

    1 record per txn      22325-22630/s  ->  39017-39057/s   +73%
    1000 records per txn      2.42-2.55M/s  ->  2.65-2.76M/s   +8%

Upstream measures +81% and +11%, so the two agree, and this is on the
filesystem where an `fdatasync` is cheapest -- which is why they expect the
EPYC to show it better still.

**I predicted 6-7% on the EPYC against upstream's expectation of more than
73%, and it came back +86%.**  Recording the prediction was worth it only
because the miss has a single cause, and it invalidates a second conclusion in
this document as well.

The prediction was: our `fdatasync` costs 12us, there are 2.005 per record, so
24us of a 181.6us row -- 13%, so one gate is worth 6-7%.  Measured, 2026-08-19,
rowid durable one record per transaction:

| | 4K | 128K |
|---|---:|---:|
| 2.2.0 (two gates) | 5486/s | 5017/s |
| 2.5.0 (two gates, control) | 5507/s | -- |
| **2.7.0 (one gate)** | **10203/s** | **9880/s** |
| | **+86%** | **+97%** |

**The 12us was not a latency. `strace -c` summarises SYSTEM time unless you
pass `-w`,** and a blocking `fdatasync` spends most of its wall time asleep,
which is not system time.  So the sweep was reporting the CPU cost of the
syscall and I read it as the cost of the gate.  `prodrun.sh` now passes `-w` at
both call sites.

The real latency is derivable from rows that measure wall time by
construction -- the paired durable and `zs_nosync` rows of the matrix itself:

    durable 1/txn   98.0us/record        nosync 1/txn   12.3us/record
    => one fdatasync = 85.7us            (and (182.3 - 12.3)/2 = 85.0us
                                          from the two-gate row: consistent)

One 86us gate then predicts the entire matrix, not just the row it was
measured on:

| records/txn | saved, measured | 85.7us / n, predicted |
|---|---:|---:|
| 1 | 84.3us/record | 85.7us |
| 10 | 7.99us/record | 8.57us |
| 100 | 1.10us/record | 0.86us |
| 1000 | 0.112us/record | 0.086us |

Four transaction sizes, two recordsizes and both table shapes all fall out of
one number, which is stronger evidence than the row itself.

**And the wall/system split is the finding the mistake was hiding.** At a
111-byte span the gate is 12us of CPU and 86us of sleep -- 14% CPU.  At a
111KB span it is 312us of CPU against 420us of wall -- **74% CPU**.  Our large
`fdatasync` is not waiting for a disk, it is *computing*: ZFS is doing zstd and
aes-256-gcm over the whole span synchronously in the caller, in the
`dmu_sync -> arc_write -> zio_write` path the perf profile shows under
`zil_commit_impl`.  2.05us/KB is about 490MB/s, which is a plausible
compress-then-encrypt rate for one core.  **Wrong, and the plausibility is how
it got in: a mechanism was fitted to a number and then written down as fact.**
A call graph shows no crypto in the caller at all; the CPU is per-block zio
setup and taskq dispatch (see "What our fdatasync actually spends its CPU
on").  Kept because the arithmetic looked convincing and was not evidence.

**Superseded on 2026-08-19: measured in wall time there is no 9x and no
deficit.** `strace -c -w` puts the two engines' slopes at 3.74 against
2.82us/KB, a factor of 1.33, on a shared ~125us ZIL floor that stock pays four
times per commit and we pay once -- so we are cheaper on `fdatasync` at every
transaction size measured.  The CPU asymmetry that remains is per-block zio
setup and taskq dispatch, not crypto.  See "The per-byte sync deficit does not
exist" and "What our fdatasync actually spends its CPU on".

#### An abort with nothing in the file writes nothing (2.7.0, C-8b)

Verified through SQL rather than taken on faith, because it is the shape a SQL
engine produces constantly -- every `ROLLBACK`, every failed statement:

    300 transactions x 10 inserted-then-rolled-back rows
      active file 432 bytes before, 432 bytes after -- nothing written

    one rollback of 60000 rows (6.6MB, past the 4MB buffer ceiling)
      file grows 7.68MB, count(*) still 1, integrity_check ok

So a rolled-back transaction that fits the buffer now costs the file nothing,
and one that does not still writes its span and voids it with a `ROLLBACK`
record that readers must honour (F-21, F-25).  The second half matters as much
as the first: it is the boundary, and it is where a writer-optional behaviour
could have been mistaken for a format change.  Nothing about our reader side
moved, and upstream's corpus case now injects the span rather than aborting to
produce it, since no conforming writer can be required to emit one any more.

**And a caller-facing change we had to answer for (C-7a).** Under two gates, a
first-gate failure guaranteed the transaction had not happened.  With one gate
a failed commit is an UNKNOWN outcome -- the terminator may or may not be
durable, and the database is correct either way.  `sqlite3BtreeCommitPhaseTwo`
inherited the old assumption by leaving `iDataVersion` alone on error, which is
"it did not happen" written in C.  It now bumps the data version and drops the
meta cache on a failed commit, so this connection re-reads rather than trusting
caches built before an outcome it cannot determine.  That is refusing to trust
our own state rather than touching the database: C-7a says report the error and
do nothing else, and in particular never retry the sync and read success as
evidence the data survived.

#### The fsync explanation was wrong, and the sweep says what replaces it

The story this document has carried -- that ZFS picks its ZIL path by write
size, so our 111KB append crosses `zfs_immediate_write_sz` (32KB) and takes the
expensive indirect path while stock's 4KB pwrites stay under it -- **predicted a
knee, and the sweep shows none.**  `fdatasync` usecs/call against per-commit
append size, 2.5.0, 4K dataset:

| records/commit | append   | zeroskip | stock |
|----------------|---------:|---------:|------:|
| 1              |   0.1KB  |    12us  |  15us |
| 10             |   1.1KB  |    13us  |  16us |
| 100            |  10.8KB  |    23us  |  16us |
| 200            |  21.7KB  |    37us  |  15us |
| 400            |  43.4KB  |    89us  |  16us |
| 1000           | 108.4KB  |   183us  |  21us |

Ours is a smooth **7us floor plus 1.65us/KB** -- that fit predicts 25/43/79/186
against 23/37/89/183 measured -- with nothing resembling a step at 32KB or
anywhere else.  Stock is flat from 111B to 176KB per commit, which the story did
get right.  So the size-scaling is real and the MECHANISM was invented.

What the sweep does show is sharper than what it refutes.  Normalising by bytes
actually synced: zeroskip pays **4.29us/KB** (43KB per commit across 2.1
fdatasyncs) against stock's **0.48us/KB** (176KB per commit across 4) -- about
**9x per synced byte**, not merely more bytes.  **Both numbers are system time
and the conclusion is retracted** -- in wall time the slopes are 3.74 against
2.82us/KB and we are cheaper per commit at every batch size; see "The per-byte
sync deficit does not exist".

**Resolved on 2026-08-19, and the units were the key.** Both sides of that
ratio are system time, so it compares CPU per synced byte, and CPU is exactly
what it turns out to be: our large `fdatasync` is 74% CPU (312us system of
420us wall), and the perf profile puts that CPU in
`zil_commit_impl -> zil_lwb_write_issue -> zfs_get_data -> dmu_sync ->
arc_write -> zio_write` -- ZFS compressing (zstd) and encrypting
(aes-256-gcm) the whole span synchronously in our thread.  **Refuted the same
day by a call graph** (see "What our fdatasync actually spends its CPU on"):
there is no crypto in the caller's stack, and `ZIO_STAGE_ISSUE_ASYNC` runs
before `ZIO_STAGE_WRITE_COMPRESS`, so both engines' crypto is on a taskq
thread.  Stock's profile is `__x64_sys_pwrite64 -> zfs_write ->
dmu_assign_arcbuf_by_dnode`, buffered, and ours is per-block zio allocation,
rangelock, znode lookup and taskq wakeup -- a different asymmetry from the one
claimed, and one that scales with the BLOCK count.  `compression=off` is
therefore no longer the decisive test; **recordsize is**, because a 130KB span
is ~32 blocks at 4K against one or two at 128K.

#### The cached small-transaction shape: no sign flip

Upstream flagged that 2.5.0 makes `rollover_txns` govern with a cache as it does
without one, taking a 200k-record load at one record per transaction from 12
conversions to 195 and 4 repacks to 65, and asked whether that order of
magnitude more file lifecycle would cost more on a pool where `unlink` is 1.8ms
than the publishing it removed.  Measured, it does not:

| 4K                        | table=none | table=local |
|---------------------------|-----------:|------------:|
| bulk, 2M at 1000/txn      |     7.65s  |      7.61s  |
| small, 20k at 1/txn       |    5491/s  |     5295/s  |
| small nosync, 200k at 1/txn |  70719/s  |    71210/s  |

128K is level on all three rows, and the 4K small-durable -3.6% is inside that
row's ~4% run-to-run spread (5507, 5491 and 5295 across this run's own arms).
Both arms show the same 195 conversions and 65 repacks, so the extra lifecycle
is being paid in both and costs nothing measurable either way.

One limit worth naming: this phase compares cached against uncached on 2.5.0,
not 2.4.0-cached against 2.5.0-cached, so it cannot isolate the 12 -> 195 change
itself.  The strict before/after exists only on the laptop, where it was faster
(1.72s -> 1.42s nosync).  What production settles is the question that matters
operationally -- enabling the cache costs nothing at any transaction size --
rather than the one that was asked.

#### 2.5.0 on production: the table is free, and the regression is closed

Measured on library 2.5.0 (74ba06a), same box and datasets.  Three
questions that had been open for several rounds all closed at once.

**The nosync 1-per-txn regression is gone, and past where it started.** That
row is the only one in either matrix that isolates per-commit CPU, which is why
it was the one that caught a 2.1.2 regression while everything else got faster:

| rowid, nosync 1-per-txn | f363402 | 2.2.0 | 2.5.0 |
|-------------------------|--------:|------:|------:|
| 4K                      |   75625 | 51505 | **79721** |
| 128K                    |   74053 | 47801 | **71480** |

+55% and +50% against the regressed version, and 4K now sits 5% ABOVE where it
was before the regression appeared.  Durable rows are flat across the same span
(5486 -> 5507 at 4K), as they must be: 2us/record hides under a 264us fdatasync.

**The pointer table now costs nothing to write, at every generation size.**
2M records at 1000-per-txn, armed, with the table against without:

| rollover | 4K | 128K | the same rows on 2.3.0 |
|----------|---:|-----:|-----------------------:|
| 2MB      | +0.5% | -0.2% | +9% |
| 16MB     |  0.0% | +0.3% | +50% |
| 64MB     | +1.0% | -0.2% | +44% |

The publish-threshold table in the raw section says the same thing from the
library's side: store time is now flat across every threshold (3625-3788ms
against 3735 for no cache) where a threshold of 1 used to cost 16613ms.

**And the two strategies converged, so the choice disappears.** Cold-import
then read with the table is now indistinguishable from importing with it --
0.167ms against 0.165ms per open, where the laptop measured 1.11x against 1.28x
on 2.4.0 -- because a commit no longer publishes at all, so both paths get
their tables from whoever replays first.  Nothing to set per phase.

**So the deployment answer simplifies to: `recordsize=128K`, the 2MB rollover
default, cascade armed, `zs_index=local` on** -- with `zs_rollover=64MB` for a
bulk import only, added after library 2.8.0 (below); it is worth -18% there and
inert for per-message commits, which trip D-9d's span bound long before any byte
bound.  The table is 1.44x on open+first-read (0.240ms -> 0.167ms) for no
measurable write cost, and the break-even arithmetic of three earlier rounds is
moot rather than merely generous.

#### What the pointer table is really worth

The 20x improvement in the raw open-cost table's PLAIN column between 2.3.0
and 2.5.0 (3.412ms to 0.166ms at 16000 records, ratio 34x to 2.3x) was worth
chasing, and upstream confirmed the cause with a sharper mechanism than the
one we guessed: **the cache was manufacturing the cost it got credit for
removing.**

An opener's replay is bounded by `min(rollover_size, rollover_txns spans)`,
and a successful publication resets the span count.  Pre-P-13 a commit
published -- so a fixture built through a threshold-1 writer published at
every commit, the span count never reached 1024, the file never sealed, and
all 16000 records sat in one unordered file.  Byte-identical layouts prove
it: at 2.3.0 the cache-configured fixture was one 2176072-byte `.current`
where the uncached one was already three sealed files, and at 2.5.0 both are
the same three files byte for byte.

**Our own numbers were touched by this too.** The 2.4.0 laptop result that
importing WITH the table beat importing cold (1.28x against 1.11x) was the
same artifact: the import-with-table database had not sealed, so its cached
arm had more to be spared.  On 2.5.0 the two are identical (0.165 against
0.167ms), which is the truth.  What made our path *mostly* immune is that
the distortion needs a plain open against a cache-built database, and only
that fixture does it: our uncached arm sealed at both versions, so our
1.47x -> 1.44x barely moved.

With the bound pinned, the honest picture is that **the governing variable
is how the caller COMMITS, not how much it has stored** -- and it is a cliff:

    16000 records, 2000 opens, span bound pinned at 1024 (laptop)

    build/txn   spans   no table   with table   speedup
    1           16000    0.137ms      0.115ms     1.19x
    40            400    1.347ms      0.092ms    14.6x
    1600           10    1.319ms      0.097ms    13.6x

Upstream measures 1.2x / 13.0x / 13.2x on the same shape, so the two agree.
The mechanism is bytes rather than spans as such: one row per commit trips
the span bound every 1024 records and seals a ~114KB tail, while a batching
writer never trips it and lets the tail reach `rollover_size` -- 2MB, about
18x more to replay.  At 100k records, where the byte bound seals regardless,
the cliff is smaller but still there (1.10x at one row per commit, 5.5x at
1600).

**For us that means the small number.** Cyrus commits per message, so the
table is worth ~1.2-1.4x on open and not the 13x a batch importer would see
-- which is exactly the 1.44x measured on production, now explained rather
than merely observed.  It is still worth enabling, because since 2.5.0 it
costs 0-1% to write.

Two cautions inherited from upstream's investigation.  **Do not reuse the
publish-threshold table** in the raw section: it was degenerate for the same
reason (every threshold publishing a 4.3KB table over an 87KB tail), and the
store column is now flat by construction, because a sole writer publishes
only at its own open.  The §1 conclusion drawn from it -- that the table's
write cost has gone -- stands on the cascade phase's own arms instead.  And
the durable lesson, which is theirs and applies to every fixture in this
tree: **a knob that bounds a quantity a fixture varies must be pinned by the
fixture, and when the feature under test can move that bound,
cached-versus-uncached is not like-for-like.** `zs_rollover_txns` exists as a
URI parameter for exactly that, and `zskvbench --opens-per N` makes the
commit shape explicit rather than implicit.

#### 2.8.0: 64MB was losing to a quadratic, and the anomaly is gone

The row this document had reported unexplained since the first rollover
sweep -- **the largest generation is the worst setting while rewriting a
quarter of the bytes** -- had a cause, and it was not visible from here.
Every delta-to-base merge in the active file's private index is O(nbase) with
a *record decode per comparison* (an index entry is an offset, so getting its
key reads the record), and it ran once per fixed 1024 inserts.  A generation
of N records therefore cost N/1024 merges of O(N): quadratic in generation
size, and `rollover_size` is what sets generation size.  Upstream counted it
exactly at 2M records -- 8.45M entries merged at 2MB, 66.4M at 16MB, **251.9M
at 64MB** -- with the merge COUNT flat and the work per merge scaling, which
is the signature.  The bound is now `max(1024, nbase/32)`, making the merge
side linear.

Confirmed here interleaved, 2M records at 1000-per-txn, rowid, three passes
per arm:

| rollover / arrival order | 2.7.0 | 2.8.0 | change |
|---|---:|---:|---:|
| 2MB, ascending | 1.24-1.41M/s | 1.28-1.41M/s | +2% (ranges overlap) |
| 2MB, random | 178-186k/s | 175-186k/s | 0% (ranges overlap) |
| **64MB, ascending** | 894-986k/s | **1.73-1.82M/s** | **+92%**, no overlap |
| **64MB, random** | 138-140k/s | **217-220k/s** | **+58%**, no overlap |

**The ranking inverts.** 64MB was 0.71x of the 2MB default on ascending keys
and is now **1.33x**; on random keys 0.77x becomes 1.24x.  So the production
rollover table above needs re-running, and the conclusion it carried for four
libraries -- that a bigger generation is simply worse on this hardware -- was
reporting a bug and not a property.

**A caution on the default arm, because the first reading was wrong.** The
2MB-random cell first came out -2.4% with non-overlapping ranges, which
looked like a small regression paid for the large win.  It was ARM ORDER: my
loop always ran 2.7.0 first, and re-running that one cell with the order
reversed puts the ranges back on top of each other (2.7.0 178-186k, 2.8.0
175-186k across both orders).  Upstream's claim that the default is inert
holds -- `nbase/32` at a 2MB generation is about 512, below the 1024 floor, so
the code cannot behave differently there -- but a 2.4% "result" with clean
separation is exactly what a fixed arm order manufactures, and interleaving
must alternate direction as well as arms.

#### Arrival order costs a fixed ~5us per record, on both machines

`zskvbench --random` stores the same key set in a scrambled order (a
multiply-mod-2^k bijection, so the records, bytes and file sizes are
identical and only arrival order differs).  It was added to test 2.8.0's
claim, and it immediately said something larger about the fixture:

    2M records, 1000-per-txn, rowid, 2.8.0, laptop

    2MB   ascending 1.34M/s      random 176k/s     7.6x
    64MB  ascending 1.82M/s      random 219k/s     8.1x

**Every store number in this document is an ascending-key number**, because a
rowid table's keys arrive in order and that is the only fixture we had.  A
real schema's secondary indexes are the other case: an index key is an
encoded column value, so its arrival order is unrelated to its sort order.

**On production the ratio is 2.5-2.9x rather than 7.6x, and the reason is the
useful part: the penalty is a nearly FIXED cost per record, not a factor.**

    2M records, 1000-per-txn, rowid, 2.8.0, 2MB rollover

                    ascending   random    ratio   penalty per record
    laptop            0.75us     5.65us    7.6x        +4.90us
    production 4K     3.72us     9.28us    2.49x       +5.56us
    production 128K   2.86us     8.20us    2.86x       +5.34us

Within 13% across two machines whose baselines differ by 5x, which says it is
CPU work proportional to nothing but the record count -- and it barely responds
to the rollover size (17.34s at 64MB against 18.55s at 2MB, where ascending
gains 44%), so it is not the index-merge path 2.8.0 fixed.  It is still not
split between the layers: our bound shortcut cannot prove absence for a random
key, so about half those inserts do a real probe with a fetch, and the
library's insert does a random-access decode per comparison in a mapped file.
Splitting it is the next measurement, and at ~5us/record against our whole
per-record overhead of 0.41-0.50us on the ascending path, it is the largest
single number left anywhere in this document.

Two things it does NOT change.  The matrix's `WITHOUT ROWID` columns are not
this effect -- those keys are `key%08d`, still ascending -- so the
duplicated-bytes explanation for that gap stands.  And Cyrus's dominant
shape is per-message commits of ascending keys, so the deployment numbers are
the right ones; this is about what the *indexes* cost, which no fixture here
has priced.

#### The cliff on ZFS, and the tmpfs index path Cyrus will use

The laptop cliff reproduces on production, with the same shape and slightly
larger ratios.  16000 records, 2000 opens, span bound pinned at
`zs_rollover_txns=1024`, one point read per open:

| build/txn | spans | 4K: no table / table | 128K: no table / table |
|---|--:|--:|--:|
| 1 | 16000 | 0.238 / 0.145ms = **1.64x** | 0.240 / 0.145ms = **1.66x** |
| 40 | 400 | 3.209 / 0.192ms = 16.7x | 3.196 / 0.182ms = 17.6x |
| 1600 | 10 | 3.105 / 0.157ms = 19.8x | 3.114 / 0.151ms = 20.6x |

So the governing variable is the commit shape on ZFS too, and Cyrus sits on
the low step: **1.6x, not 17x.**  Worth having, since it costs nothing to
write, but it is not the number to plan around.

**The tmpfs index path buys nothing measurable over the on-pool cache.** This
is the shape Cyrus will actually run, since `/tmpfs` is mounted on the
production boxes, so it was worth measuring rather than assuming:

| 20000 records, 2000 opens, one per txn | 4K | 128K |
|---|--:|--:|
| no cache | 0.234ms | 0.234ms |
| `zs_index=local` (cache on the pool) | 0.162ms | 0.164ms |
| `zs_index_dir=/tmpfs/zsidx` | 0.162ms | 0.159ms |
| stock btree, for scale | 0.048ms | 0.056ms |

Steady-state opens are identical to three digits; only the FIRST open differs,
and slightly (0.377/0.369ms on tmpfs against 0.420/0.461ms on the pool),
because that is the open that writes the table.  The case for tmpfs is
therefore not open latency -- it is keeping the cache's writes and its unlinks
off the pool entirely, which this benchmark does not price, and accepting that
the cache is rebuilt after a reboot, which costs one 0.4ms open per database.
Both are fine for Cyrus.  **Stock's open is still 3x cheaper than our best**,
which is the one open-side number that has not moved all project and is the
honest cost of replaying a tail instead of reading a root page.

#### 2.5.0 made the pointer table nearly free to write

Every figure in the next section for the table's WRITE side is superseded, and
the reason is worth keeping because it is the same shape as three earlier
corrections.  Library 2.5.0 stopped publishing a table at commit: publishing
amortises a replay, the commit path folds rather than replaying (D-13b), so a
table written there cost the writer and bought it nothing.  Interleaved here,
2M records at 1000-per-txn with the cache on: **1.86-1.95s -> 1.47-1.53s,
-21%**, and now level with running no cache at all (1.46-1.57s).

If that holds on ZFS -- where the table cost +9% at the 2MB default and
+44-50% at a large generation -- then the break-even arithmetic in the next
section collapses: an open-side gain of 1.4-1.5x against no write-side cost
needs no ratio at all, and the deployment answer becomes "turn it on" rather
than "set it per phase".  `prodrun.sh`'s `cached` phase measures it.

**And a caller-facing consequence upstream flagged rather than buried.** A
writer no longer moves its own replay window, so with a cache configured
`rollover_txns` governs exactly as it does without one, and a 200k-record load
at ONE record per transaction goes from 12 conversions to **195**, with repacks
4 -> 65.  That is an order of magnitude more file lifecycle for the same data.
Here it is still faster -- 10.06s -> 8.86s durable, 1.72s -> 1.42s nosync,
because the publishing it stopped doing cost more than the extra sealing -- but
`unlink` is 1.8ms on our pool against nothing on APFS, and the nosync run is
short enough that a second of unlinks would dominate it.  So this is the row
where the sign could flip, and if it does, upstream has already named the
answer: let the writer publish rarely, at something like file/8, purely to
advance its own window.  Measuring it is ours.

#### Open latency, and what the pointer table is worth to us

Nothing else in this tier measured the metric a Cyrus-shaped deployment
actually lives on: many short-lived processes, each opening the database,
reading a little and exiting.  `zskvbench --opens N` does -- it builds the
database ONE record per transaction, because that is what accumulates the
spans a snapshot open replays, then times N open+point-read+close cycles.

    20k records built one-per-txn, 2000 read-only opens, best of 3, laptop,
    rowid, library 2.4.0

    import cold, read cold           0.142ms per open   (first 0.279ms)
    import cold, read with table     0.128ms  1.11x     (first 0.475ms)
    import WITH table, read with it  0.111ms  1.28x     (first 0.241ms)
    stock btree                      0.030ms  4.5x faster than any of them

Three readings, and the last is the uncomfortable one.

**The table's open-side prize is real but smaller than the library's**, 1.28x
against upstream's 2.4x bound for an idle 1000-span file, because SQLite's own
open is a fixed floor the table cannot touch -- stock pays 0.030ms without
any replay at all.

**There are two ways to get it, and they have different prices.** Enabling the
flag on a read-mostly database AFTER an import does work as of library 2.4.0,
which made a read-only handle CREATE the cache directory rather than silently
running uncached when it was absent; before that the case did nothing until
some unrelated write came along.  Verified end to end here: a cold import
leaves no cache, one read-only open with `zs_index=local` leaves
`zeroskip.cache/zeroskip.index-<uuid>-00000004` -- a table for the active
generation, which is where the spans are.  That path recovers about half of
what the writer-published path does (1.11x against 1.28x) and the first reader
pays ~0.35ms extra to publish, but it costs the import NOTHING.  So:

    A  table on during the import     +0.76s on a 2M-record load, 0.031ms/open
    B  import cold, table for reads   +0 on the load,             0.014ms/open

**B beats no table from the very first open, and A only overtakes B past
~45,000 opens per import** (0.76s / 0.017ms).  My earlier break-even of 24,000
opens was for A against no table, and it quietly assumed tables would be there
to be read -- which for the case that matters they were not until 2.4.0.  For
a mailbox database B is the obvious default: no write-side cost, and the read
side improves from the first open onward.  Set it per database and per phase,
which the URI already allows.

**On production the table is worth more and costs less than the laptop
suggested**, which moves the break-even by a factor of two and a half.  20k
records built one-per-txn, 2000 read-only opens, strategy A (table present):

    4K     no table 0.242ms   with table 0.165ms   1.47x   stock 0.048ms
    128K   no table 0.238ms   with table 0.170ms   1.40x   stock 0.059ms

So the table saves **0.077ms per open** there against 0.031ms here, because a
ZFS open pays more for the replay it removes.  Against its measured cost of
+9% on a 2M-record load (0.72s), break-even for strategy A is **~9,400 opens
per bulk load of that size** rather than 24,000.  For a mailbox database that
is trivially met.  The cold-import arm (strategy B) has not been measured on
production yet -- it needs 2.4.0, which this vendor bump supplies, and the
three-arm `opens` phase now in `prodrun.sh`.

Open+first-read remains our weakest read metric against stock on production
too: **0.24ms against 0.048ms, 5x**, and 3.4x even with the table.

**Open+first-read is our weakest read metric against stock -- 4.5x, worse
than scans.** It had not been measured before this because no workload here
opened more than once per phase.  For short-lived writers it is the number
that matters most, so it belongs in the matrix rather than in a footnote, and
`prodrun.sh` now has a phase for it.

#### What the raw column flags next, and what it does not

Three things the same run says, kept here so they are not re-derived:

- **The undo log's before-image fetch is worth 14-38% of multi-row DML,
  and the cost is the LOOKUP, not the write-buffer flush.**  Both halves are
  now taken where they can be: -17% on `INSERT ... SELECT` from the append
  bound, -16% on a wide-row point update from the cursor.  What remains is
  the scan-driven shapes, declined on memory grounds.  See below; this
  bullet used to say the opposite.
- **`rollover_size` IS the knob, and the earlier reading here was another
  measurement taken where the mechanism does not exist.** This bullet used
  to say it was not worth exposing, on the strength of a 16k/64k/256k/2048k
  sweep at 20k records where the numbers were flat -- a database too small
  for the repack cascade to run at all, which is the same error the
  recordsize claim made.  At 2M records it is the largest lever either side
  has found; see below.
- **Compaction is where read throughput comes from, and it is cheap
  here**: 7-14ms for a 2.5MB database, reclaiming 28/55/78% of the file
  at 25/50/75% deleted.  It is still unbounded, which is why VACUUM is
  the only thing that calls it.
- **A shared leaf symbol needs a call graph, not a flat table.**
  `test/zs/attribute.py` walks `sample`'s call graph and attributes a leaf
  to the nearest enclosing subsystem frame.  It came from upstream after we
  each misread the same profile in opposite directions, and it is the tool
  to reach for before quoting a percentage for `zsi_rec_decode`, `memcmp`
  or `memset` -- the last of which turns out to be 99.7% the repacker.

### The undo log's before-image fetch (2026-08-17)

`zsbtWrite` fetches a row's current version before overwriting it, so a
savepoint rollback can put it back.  `test/zs/undobench.sh` is the
benchmark that can see it; instrumenting the funnel is what established
when it runs at all, and both answers were surprises.

**It is not about savepoints.**  A mark is open whenever SQLite opens a
statement journal inside an explicit transaction, and that covers
ordinary multi-row DML.  Counted per transaction, 20k rows:

| statement                    | mark open? | fetch result |
|------------------------------|------------|--------------|
| prepared 1-row INSERT (BEGIN/COMMIT) | no  | never runs   |
| `INSERT ... SELECT`          | **yes**, no savepoint needed | misses |
| `UPDATE t SET ...`           | **yes**, no savepoint needed | hits |
| `DELETE FROM t` (truncate path) | only inside a SAVEPOINT | hits |

So `zskvbench` never touches this path -- a prepared single-row insert
needs no statement journal -- which is why the cost was invisible, and
why the doc could previously say "one durable fetch in the whole
transaction" while an `UPDATE` was paying one per row.  It also means
wrapping a statement in `SAVEPOINT` costs almost nothing beyond what the
statement journal already spends: the sp/nosp pair in that script reads
+3% for update, and both arms log.

**The prize, measured** by patching the fetch out (an unsound build:
rollback replays nothing) at 500k rows, user CPU:

| shape                        | base  | no fetch | prize | per row |
|------------------------------|------:|---------:|------:|--------:|
| `INSERT ... SELECT`          | 0.36s |   0.30s  | -17%  |  120ns  |
| `UPDATE`                     | 0.70s |  0.565s  | -19%  |  270ns  |
| insert then update, one txn  | 0.88s |   0.76s  | -14%  |  120ns  |
| `DELETE FROM` in a savepoint | 0.42s |   0.26s  | -38%  |  320ns  |

**No flush cost appears anywhere**, which retires the lead this started
from.  `zsi_txn_at` flushes the append buffer only when the bytes sought are
still buffered (`need > txn->flushed`), and a SQL statement rewrites keys
in bulk, so the buffer flushes once rather than per row -- the 5.3x
`store+read back` figure is a per-record read-back pattern this layer does
not produce.  `ZS_EPHEMERAL` plus a copy, which upstream unblocked on
2026-08-17, therefore buys nothing measurable here and would add a memcpy
per before-image.  Not worth doing, and worth saying so.

#### The miss case, done: prove absence from the bound

A valid bound names a live key with nothing above it, so a key STRICTLY
above it has no current version and there is nothing to save.  That is the
whole of the miss case, and it is now what `zsbtWrite` does.  Interleaved
A/B on clean pre- and post-change binaries, 500k rows, min user CPU:

| shape                        | before | after  | change | ceiling |
|------------------------------|-------:|-------:|-------:|--------:|
| `INSERT ... SELECT`          | 0.36s  | 0.30s  | **-17%** | 0.30s |
| insert then update, one txn  | 0.88s  | 0.815s | **-7%**  | 0.76s |
| `UPDATE`                     | 0.715s | 0.715s | flat     | 0.565s |
| `DELETE FROM` in a savepoint | 0.41s  | 0.415s | flat     | 0.26s  |

The insert row captures the entire available prize -- "after" equals the
patched-out ceiling to the resolution of the measurement.  The rewrite row
gets its insert half only, which is the design: its update half rewrites
keys at or below the bound, where the before-image is real.  Update and
delete are untouched for the same reason.

Two things this change is careful about, both mutation-checked:

- **STRICTLY above.**  A key EQUAL to the bound is present, and recording
  it as absent makes a rollback DELETE that row instead of restoring it.
  `>=` in place of `>` fails `05-savepoints`, `10-boundexact` and
  `11-undoskip`.
- **The funnel now maintains what it consumes.**  The shortcut is sound
  only if every insert raises the bound -- otherwise a key inserted above
  a stale bound looks absent when it is written a second time, and a
  `ROLLBACK TO` between the two writes deletes the row instead of
  restoring the first version.  `sqlite3BtreeInsert` used to do the
  raising, in two places, outside the funnel; that is now one raise inside
  `zsbtWrite`, sharing the single slot lookup with the shortcut.  Removing
  it fails five batteries.

`test/zs/11-undoskip` is the battery, written before the change and passing
on the unchanged engine first.  Sections 6 and 7 cover the shape nothing
else here reaches: a UNIQUE violation part-way through an
`INSERT ... SELECT` with no savepoint anywhere, where the statement journal
is what opens the mark and earlier rows of that statement must be undone
while the rest of the transaction survives.

#### The hit case: the before-image from the cursor

`zsbtWrite` now takes an optional cursor whose payload may already BE the
before-image, and `zsbtUndoHint` decides whether it can be believed.  A
point `UPDATE` has seeked the row and read it; fetching it again repeats
that lookup.  **-16% on a wide-row point update** (0.35s -> 0.29s, 200k
rows, three interleaved passes), which is the whole of the ceiling for
that shape.

Two things about this were wrong when it was proposed, and both were found
by measuring instead of reasoning.

**A cursor borrow is not a transaction borrow.**  A-4 gives a pointer the
lifetime of *whatever produced it* -- a fetch borrow lives as long as the
transaction, a cursor borrow only as long as that cursor -- and this engine
finalises cursors on insert, delete and re-seek, while an undo entry
outlives all three.  Four of the five `zsbtLoadCurrent` call sites are
cursor steps, so the flag `valFromFetch` records which kind a payload is
and the hint refuses the rest.  That is why a SCAN-driven
`UPDATE t SET b=...` gets nothing from this: instrumented, all 20000 of
its writes are refused for exactly that reason, and correctly.
(A-4a also settles a question this file had open in the other direction:
it binds borrows across a snapshot swap, so a rollover mid-transaction does
NOT invalidate them.  `zsbtLoadCurrent`'s comment said the opposite and
called it queued upstream; it is answered, and the existing undo log's
transaction-lifetime borrow was sound all along.)

**"The UPDATE has already read the row" is not always true.**  SQLite reads
only the columns it needs, so `UPDATE t SET b=?` on `t(a INTEGER PRIMARY
KEY, b)` reads NOTHING from the row -- the key comes from the seek and `b`
is being replaced -- and the value is never loaded.  Instrumented, all
20000 writes of that statement are refused at the `valNotLoaded` guard, and
the hint is worth nothing.  Add a column the statement must preserve, which
is what any real schema has, and it fires on every row.  The measurement
above is that shape for that reason; a point `DELETE ... WHERE a=?` reads
nothing either and is likewise flat.

Four guards, each independently mutation-checked and each caught by
`test/zs/11-undoskip` alone: no key comparison (`OP_NewRowid` leaves the
cursor on the largest key while the insert goes to a new one), no epoch
check, ignoring `ZS_VAL_ABSENT`, and accepting cursor-step borrows.

**And a layout constraint nobody knew was there**: exactly eight `u8`
fields may precede `pBtree` in `BtCursor`.  `test3.c`'s `btree_*` TCL
commands reach into the struct through `btreeInt.h`'s STOCK definition,
where four `u8`s and an `int` put `pBtree` at offset 8; our eight `u8`s
land it in the same place, and that coincidence is the only thing keeping
those commands off a wild pointer.  Adding a ninth for this change pushed
`pBtree` to 16 and segfaulted `fordelete.test` and `types.test` -- the
first two failures the TCL permutation has produced in months, and they
came from a field, not from logic.  The provenance flag is therefore packed
into one `valSrc` field (`ZS_VAL_CURSOR`/`ABSENT`/`FETCH`) rather than
added beside `valNotLoaded`, and the constraint is now commented on the
struct.

What is left of the original 19-38% is the scan-driven bulk shapes, and
they are NOT worth taking: the only way to use a cursor-step borrow is to
copy it, and for `DELETE FROM t` inside a savepoint that means duplicating
the whole table in heap where today the undo log borrows bytes that are
already on disk.  Trading tens of megabytes of RSS for tenths of a second
of CPU is the wrong direction for a mail server, so the remaining prize is
declined rather than pending.

### Where the store row actually goes

`sample` over the 1000-per-txn row, rowid shape, leaf symbols: it is
LOOKUP, not writing.  `zsi_rec_decode` 858, memcmp 812, `zsi_index_lb`
603, `zsi_pend_lb` 487, the per-fetch cursor setup (`zsi_cursor_open`,
`zsi_fcur_load`, `zsi_cur_sort`) 396, checksums 395 -- against `write`
310 and `sqlite3VdbeExec` 79 out of ~5200 samples.  So roughly 60% of
the row is the per-row PROBE and about 7% is the writing, and the SQL
machinery the engine is often blamed for is noise.

(That profile is HISTORICAL: measured on library f363402, before both
`TableMoveto` changes and before 2.1.2 halved the pending-set walk.  It is
kept because it is the evidence that led to the work below, not as a
description of the engine today.  The current at-scale library profile is
in the section above.)

Every INSERT with an explicit key makes SQLite emit `OP_NotExists`, and
each probe costs O(files): open a cursor over the file set, load and
sort the arms, binary-search and decode within each.  A bulk load
creates generations as it goes, so it makes its own probes more
expensive as it runs.

`sqlite3BtreeTableMoveto` used to pay that TWICE on a miss -- a
`ZS_FETCHNEXT` to discover the key is absent, then a `ZS_FETCHPREV` to
land on the entry below.  Both are gone now, in two steps.

First the forward fetch, whose answer the bound already knows: +29% at
1000-per-txn, +18% at 100.  Then the backward one, by making the bound
EXACT rather than stale-high -- `zsbtWrite`, the funnel every mutation
passes through, drops it when a delete removes the key it names -- so
the bound IS the largest live key, which is precisely the entry a miss
above it must land on.  Positioning from it costs nothing, and the value
is left to `valNotLoaded`, which fetches durably only if something reads
the row.  A further +55% at 1000-per-txn (1.19M -> 1.86M/s) and +26% at
100.  Write transactions only, because that is the only place the bound
exists; single-row transactions, `WITHOUT ROWID`, fetch and scan are all
flat.  On production hardware the two of them plus the meta cache come
to +41% at 100-per-txn and +77% at 1000 (paired table above).

The intermediate design -- DEFERRING the positioning fetch until
something reads the cursor -- does not work, and the reason is worth
recording so nobody re-derives it.  For `OP_SeekLE`/`OP_SeekLT` with
`res<0` the VDBE reads the row WITHOUT stepping, and it distinguishes
"positioned below a real row" from "empty table" by calling
`sqlite3BtreeEof`, which returns an `int` and has no way to report an
error.  A deferred position could only answer it by fetching there and
swallowing any I/O failure, or by inferring emptiness from the bound,
which was unsound while the bound was stale-high.  `test/zs/09-rowidseek`
contains that case and a mutant that defers the fetch fails it on three
lines.  Exactness is what removed the fetch instead of hiding it.

`test/zs/10-boundexact` guards the invariant the exact bound rests on:
every way of removing or moving a tree's largest key -- a delete, an
UPDATE that moves a rowid, `DELETE FROM` with no WHERE (which sweeps
key by key through the same funnel), a savepoint rollback that restores
one -- must drop the bound, or a deleted row comes back to life through
a cursor positioned on it.  Three mutants: no invalidation, `>` instead
of `>=` (deleting exactly the bound's key), and positioning from an
empty tree's zero-length bound.  All three fail that file and none of
the other nine notice, which is why it is its own file.

**recordsize: an earlier version of this document said it barely
matters, and that was an artifact of measuring a database too small to
repack.** The 2026-08-16 sweep (4K, 128K and 1M, pre-meta-cache engine, 20k
records) found 4K and 128K within 0.1-1.5% on the 1000-per-txn rows, and
1M *worse* -- mildly for the btree (408782/s at 1M against 455997 at 4K)
and by 28% for zeroskip on rowid tables (181462 against 252058).  At 20k
records the whole database is a few MB, lives in ARC, and the cascade
never runs, so what that sweep compared was the cost of appending, where
1M's read-modify-write unit is pure overhead.

The 2026-08-18 matrix says the opposite once the cascade is in play:
128K beats 4K by 11% at 20k records and by **36%** at 2M, because a
cascade's rewriting is large and sequential -- 830MB of merges take
3696ms at 4K and 1939ms at 128K.  Both readings are right about
different workloads, and the deployment answer is 128K unless the
database stays small enough never to repack.

Point fetches beat the stock page cache by ~1.4x on real hardware, where
the laptop showed rough parity.

Full scans are stock's on production, by 1.6x (WITHOUT ROWID) to 1.9x
(rowid) -- but that row is the least stable number in this document and
should not be used to plan work.  The two engines have differently
shaped scan costs: stock's tracks the payload (its cost is pages walked),
ours is mostly per-row.  So which one wins flips with value size and
machine.  On the laptop at 200k rows we scan 25.8M/s against stock's
19.9M with 100-byte values, and 27.8M against stock's 56.3M with 8-byte
values -- we win by 1.3x and lose by 2x on the same build, from one
parameter.  Raw zeroskip also scans nearly 2x faster than stock's page
walk on production (20.3M vs 11.1M), so a scan through the merge is not
inherently the slower structure.  A scan comparison worth acting on has
to fix the row size and the machine and say so.

Development-laptop numbers (Apple Silicon/APFS, 20k records, 100-byte
values, best of 3, zeroskip @ 70aabc494ff1, after the engine's
fewer-point-ops-per-row optimisation) — secondary, and note that macOS
`fsync()` does not force a device cache flush without `F_FULLFSYNC`,
so every durable row below flatters both engines.  "zs nosync" is the
zeroskip analog of WAL/NORMAL:
`file:PATH?zs_nosync=1&zs_sync_ms=1000` opens the handle ZS_NOSYNC and
issues a periodic zs_db_sync after commits, bounding the loss window
to ~1s; like WAL/NORMAL it trades durability-at-commit away, unlike
default zeroskip and stock-journal which are durable at every commit.
As of upstream C-6b (c945dc0), ZS_NOSYNC skips ONLY the two per-commit
gates -- creation/conversion/repack syncs always run -- so a crash
loses at most the unconverted tail of the active generation and never
structure (verified here: kill -9 under zs_nosync leaves
integrity_check ok with all-or-nothing content).

| workload            | raw zeroskip | SQL on zeroskip | zs nosync 1s | stock (journal) | stock (WAL) |
|---------------------|-------------:|----------------:|-------------:|----------------:|------------:|
| store, 1 per txn    |      20072/s |         17392/s |      66928/s |          4842/s |    109013/s |
| store, 10 per txn   |     165915/s |        125034/s |     291024/s |         44118/s |    452509/s |
| store, 100 per txn  |     660874/s |        509710/s |     736243/s |        257030/s |   1218389/s |
| store, 1000 per txn |    1661538/s |        860220/s |     923317/s |       1423704/s |   2164970/s |
| point fetch         |     ~318000/s |        241295/s |     242777/s |        253569/s |    764854/s |
| full scan           |      18.0M/s |         18.9M/s |      17.5M/s |         11-36M/s |     27-38M/s |

Readings (after two rounds of co-evolution with upstream: their probe
directory stream, cached append descriptor, single-write spans, and
inclusive-GE ZS_FETCHNEXT; our movetos as single point fetches with
lazy cursors, seeded append bounds, scratch-buffer encodes): every
store row keeps the intuitive raw-above-SQL ordering, the SQL layer
tracks raw within ~25% through 100/txn and ~2x at 1000/txn, and point
fetches through the full SQL stack now match the stock btree's page
cache (241k vs 254k/s) while raw zeroskip fetches run 318k/s.  At
equal durability the zeroskip engine beats stock-journal 3.6x at
1-per-txn; in the reduced-durability pairing, zs-nosync reaches ~60%
of WAL/NORMAL on single-statement stores and ~75% at 100/txn.  Scan
numbers vary run-to-run with thermal state; stock's packed pages keep
a structural scan edge.

File state and checksums (200k records, one rep, through SQL):
compaction is the big read lever -- VACUUM (which now performs a real
zs_db_compact after its rebuild commits; backup destinations compact
on finish) takes point fetches from 190k/s to 372k/s, PAST stock's
277k/s, because point-lookup cost is proportional to the file count
(D-14d).  Read-side checksum verification costs ~12% of scan
throughput through SQL and nothing measurable on fetches; `zs_nocsum=1`
skips verification and `zs_csum=none` writes engine-0 files (both URI
parameters, both opt-outs of a safety net the stock btree never had).
Library 2.1.0's own `full scan, no verify` row prices the same thing
underneath us, and on production hardware it is much larger there --
19.6M against 25.1M records/s at 4K, so **22% of a RAW scan** -- which is
consistent rather than contradictory: our scan spends most of its time in
the VDBE, so the same absolute saving is a smaller share of it.
As of upstream F-5e (266320e), ZS_NOCSUM skips only record-checksum
verification at materialization -- span/terminator checksums are
always verified -- so even zs_nosync+zs_nocsum together stays
crash-safe: a reopen after a crash yields a valid prefix.
Compacted, unverified scans reach 26.4M/s vs stock's 31M/s; the
residual is record decode and merge stepping against packed page
cells.

History: an earlier vendored snapshot had per-commit cost growing
linearly with active-file size (a snapshot rebuild on every write-txn
begin), which made these small-txn rows look inverted — SQL "beating"
raw because fatter records rolled the active file over sooner.  Found
via this harness, fixed upstream in 7d11393 ("a write begin reuses a
fresh snapshot via the C-4i probe"); raw small-txn stores went from a
6000->800/s sawtooth to ~13k/s flat, and the SQL layer inherited the
same win.

## Testing

```
make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
     lib sqlite3zs zskey-test zsbtree-test
./zskey-test                    # codec property test
./zsbtree-test                  # engine-core C harness
./test/zs/run-tests.sh .        # SQL batteries (stock output = ground truth)
./test/zs/crash-test.sh .       # kill -9 recovery, 10 rounds
./test/zs/format-guard.sh .     # what an unreadable database does
./test/zs/busy-test.sh .        # two-process locking
./test/zs/backup-concurrent.sh .
```

The SQL batteries' `.expected` files are generated by running the same
SQL through the stock `./sqlite3` shell — the two engines must agree.

SQLite's own TCL suite runs against the engine through the `zeroskip`
permutation:

```
make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' \
     'LDFLAGS.libsqlite3=-L/opt/homebrew/lib -rpath /usr/local/lib -lz' \
     lib testfixture && rm -f testfixturezs && cp testfixture testfixturezs
./test/zs/run-suite.sh .        # one process per file, per-file scratch dir
```

Two TCL files in `test/zs/` are NOT in the permutation and so are not
run by either script above -- they are engine-specific shapes with no
home in SQLite's own suite, and they have to be named explicitly:

```
rm -rf /tmp/zsrun && mkdir -p /tmp/zsrun && cp testfixturezs /tmp/zsrun/
cd /tmp/zsrun && ./testfixturezs $TOP/test/zs/twowriter.test   # two connections, one process
cd /tmp/zsrun && ./testfixturezs $TOP/test/zs/metacache.test   # meta cache invalidation paths
```

The isolated runner matters: a zeroskip database is a *directory*, and
several tests' single-file cleanup idioms cannot remove one, so leftover
state from an earlier file changes a later file's result.  Excluded
files and per-test guards are ledgered one line each in
`doc/zeroskip-testsuite.md`.

## Re-vendoring zeroskip

The engine has NO dependence on zeroskip's on-disk format: it never
reads a file name, counts files, or knows the layout.  The only
directory-level code is the recursive delete for ephemeral databases.
A format change is therefore a re-vendor and a re-verify, not a port.
What to run, in order: a clean rebuild (`rm -f *.o libsqlite3.a` --
make cannot see that OPTIONS changed), `zskey-test`, `zsbtree-test`,
the SQL batteries, `format-guard.sh`, `crash-test.sh`, `busy-test.sh`,
`backup-concurrent.sh`, then the full tier via `run-suite.sh`.

The one thing a format change can break silently is a database written
by the previous version: `format-guard.sh` records that an unreadable
database currently opens as an EMPTY one rather than failing, so an old
database met by a new library would look empty and the first write
would start a fresh generation beside data still on disk.  If upstream
makes a version mismatch a hard error, that test changes and should be
updated to match -- deliberately.

`ext/zeroskip/VENDOR` records the upstream commit.  To re-vendor:
copy `zeroskip.c`, `zeroskip.h`, `xxhash.h` from `../zeroskip2`, update
VENDOR, rebuild, and run the full test list above.

The savepoint undo log borrows before-image value pointers under A-4's
transaction-lifetime rule, including fetches of the transaction's own
uncommitted records — an earlier upstream bug clobbered those on
re-store and was fixed in zeroskip2; a re-vendor that regresses it
shows up as savepoint-battery corruption (05-savepoints).
