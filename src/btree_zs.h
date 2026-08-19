/*
** 2026-08-12
**
** Internal interface between the zeroskip btree engine (btree_zs.c)
** and the zeroskip backup implementation (backup_zs.c).  Only compiled
** under SQLITE_ZEROSKIP.
*/
#ifndef SQLITE_BTREE_ZS_H
#define SQLITE_BTREE_ZS_H

struct zs_db;
struct zs_txn;

/* The underlying zeroskip handle, opening it if necessary.  Returns 0
** if the database cannot be opened. */
struct zs_db *sqlite3ZsBtreeDb(Btree *p);

/* The btree's open transaction, or 0. */
struct zs_txn *sqlite3ZsBtreeTxn(Btree *p);

/* Bump the write epoch and data version after content changed behind
** the btree layer's back (backup into an open handle). */
void sqlite3ZsBtreeInvalidate(Btree *p);

/* Set or clear the sqlite3BtreeIsInBackup flag. */
void sqlite3ZsBtreeSetInBackup(Btree *p, int flag);

/* Schedule a full zs_db_compact after the current write txn commits. */
void sqlite3ZsBtreeScheduleCompact(Btree *p);

/* The library's rewrite counters for a connection's database (A-17), for
** benchmarks that want to report their own write amplification.  Fills
** aOut[0..7] with, IN THIS ORDER: repacks, repack_records, repack_bytes,
** repack_ns, conversions, convert_records, convert_bytes, convert_ns.
**
** Flattened rather than passing struct zs_db_stats through, so a caller
** needs neither zeroskip.h nor an include path for it.  Counters are per
** HANDLE and monotonic since open, so another process's repacks are
** invisible -- and a compaction counts as a repack, because that is what
** it is.  zDb may be 0 for "main". */
int sqlite3ZsStats(sqlite3 *db, const char *zDb,
                   sqlite3_uint64 *aOut, int nOut);

/* Drive the repack cascade from outside the write path, for a handle opened
** with zs_norepack=1.  Not inside a transaction -- a merge takes the write
** lock.
**
** nMaxMerges bounds the work: at most that many merges, or unbounded when it
** is <=0.  A caller with a latency budget wants a small bound, because a
** merge's cost is a whole generation and it cannot be interrupted once
** started -- an unbounded call can stall a worker for seconds on a database
** that has fallen behind.  That is the shape a delayed-work slot needs, and
** the reason this is not simply a loop the caller could write: without
** *pbBehind it cannot tell "done" from "gave up early".
**
** *pnMerges, if given, receives the number of merges run.  *pbBehind, if
** given, receives true when more merges remain -- so a caller can schedule
** itself again rather than guess.  Never disarm and never ignore *pbBehind:
** point-lookup cost is linear in the file count (D-14d), and a 2M-record load
** left un-repacked leaves 119 files and takes readdir from 6,378 calls to
** 29,723. */
int sqlite3ZsRepackCatchUp(sqlite3 *db, const char *zDb, int nMaxMerges,
                           int *pnMerges, int *pbBehind);

/* Convert the active generation now (zs_db_seal), rather than leaving it to
** whichever commit grows the file past rollover_size.  Not inside a
** transaction.  A no-op when there is nothing to seal.
**
** This is the latency lever for CONVERSIONS, and it is the one that matters
** for a caller that commits per message.  zs_norepack/RepackCatchUp move the
** repack cascade off the write path and leave conversions on it: measured at
** 20000 single-record commits, 19 of the 25 slow commits survive disarming the
** cascade, and the counters say they are conversions.  A conversion is
** unavoidable once per generation, but WHEN it happens is not -- calling this
** from an idle moment means the commit that would have paid for it has nothing
** to do.  Its size is bounded by rollover_size (D-12d), so that knob sets how
** tall the outlier can be and this call sets when it lands. */
int sqlite3ZsSeal(sqlite3 *db, const char *zDb);

#endif /* SQLITE_BTREE_ZS_H */
