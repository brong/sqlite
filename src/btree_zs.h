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

/* Drive the repack cascade to completion, for a handle opened with
** zs_norepack=1 (which keeps it off the write path).  Not inside a
** transaction.  *pnMerges, if given, receives the number of merges run. */
int sqlite3ZsRepackCatchUp(sqlite3 *db, const char *zDb, int *pnMerges);

#endif /* SQLITE_BTREE_ZS_H */
