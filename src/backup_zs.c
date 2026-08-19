/*
** 2026-08-11
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Online backup API for the zeroskip storage engine.  Compiled in place
** of backup.c when SQLITE_ZEROSKIP is defined.
**
** A zeroskip shared transaction is a stable snapshot of the source, so
** the backup is simply: snapshot the source, clear the destination
** inside a write transaction, copy every record, commit.  Unlike the
** stock pager backup, concurrent writes to the source never invalidate
** or restart the copy -- the result is consistent as of the first
** step.  An unfinished backup aborts the destination transaction and
** leaves the destination untouched.
*/
#ifdef SQLITE_ZEROSKIP
#include "sqliteInt.h"
#include "btree.h"
#include "btree_zs.h"
#include "zeroskip.h"

struct sqlite3_backup {
  sqlite3 *pDestDb;             /* destination connection */
  Btree *pDest;
  sqlite3 *pSrcDb;              /* source connection */
  Btree *pSrc;
  struct zs_txn *pSrcTxn;       /* shared snapshot, opened on first step */
  struct zs_txn *pDestTxn;      /* write txn on the destination */
  struct zs_cursor *pCur;       /* walk of the entire source keyspace */
  int rc;                       /* sticky result */
  int nRemaining;
  int nPagecount;
};

static int zsbkErr(int zsrc){
  switch( zsrc ){
    case ZS_OK:          return SQLITE_OK;
    case ZS_LOCKED:      return SQLITE_BUSY;
    case ZS_READONLY:    return SQLITE_READONLY;
    case ZS_BADFORMAT:
    case ZS_BADCHECKSUM: return SQLITE_CORRUPT_BKPT;
    case ZS_FULL:        return SQLITE_FULL;
    case ZS_IOERROR:     return SQLITE_IOERR;
    default:             return SQLITE_INTERNAL;
  }
}

/* Find the Btree for database zDb in connection pDb (cf. stock
** backup.c findBtree). */
static Btree *zsbkFindBtree(sqlite3 *pErrorDb, sqlite3 *pDb, const char *zDb){
  int i = sqlite3FindDbName(pDb, zDb);
  if( i==1 ){
    Parse sParse;
    int rc = 0;
    sqlite3ParseObjectInit(&sParse, pDb);
    if( sqlite3OpenTempDatabase(&sParse) ){
      sqlite3ErrorWithMsg(pErrorDb, sParse.rc, "%s", sParse.zErrMsg);
      rc = SQLITE_ERROR;
    }
    sqlite3DbFree(pErrorDb, sParse.zErrMsg);
    sqlite3ParseObjectReset(&sParse);
    if( rc ) return 0;
  }
  if( i<0 ){
    sqlite3ErrorWithMsg(pErrorDb, SQLITE_ERROR, "unknown database %s", zDb);
    return 0;
  }
  return pDb->aDb[i].pBt;
}

/* Delete every record in the destination inside pDestTxn. */
static int zsbkClearDest(sqlite3_backup *p){
  struct zs_cursor *pZc = 0;
  const char *k, *v;
  size_t nk, nv;
  int zrc;

  zrc = zs_txn_begin_cursor(p->pDestTxn, 0, 0, &pZc, 0);
  if( zrc!=ZS_OK ) return zsbkErr(zrc);
  while( (zrc = zs_cursor_next(pZc, &k, &nk, &v, &nv))==ZS_OK ){
    zrc = zs_txn_store(p->pDestTxn, k, nk, 0, 0, 0);
    if( zrc!=ZS_OK ) break;
  }
  zs_cursor_fini(&pZc);
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbkErr(zrc);
  return SQLITE_OK;
}

/* First step: snapshot the source, count its records, open and clear
** the destination. */
static int zsbkBegin(sqlite3_backup *p){
  struct zs_db *pSrcZs, *pDestZs;
  struct zs_cursor *pZc = 0;
  const char *k, *v;
  size_t nk, nv;
  int zrc;
  int rc;
  int n = 0;

  pSrcZs = sqlite3ZsBtreeDb(p->pSrc);
  pDestZs = sqlite3ZsBtreeDb(p->pDest);
  if( pSrcZs==0 || pDestZs==0 ) return SQLITE_CANTOPEN_BKPT;

  zrc = zs_db_begin_txn(pSrcZs, 1, &p->pSrcTxn);
  if( zrc!=ZS_OK ) return zsbkErr(zrc);
  zrc = zs_db_begin_txn(pDestZs, 0, &p->pDestTxn);
  if( zrc!=ZS_OK ){
    zs_txn_abort(&p->pSrcTxn);
    return zsbkErr(zrc);
  }

  /* advisory progress numbers: one pre-scan to count */
  zrc = zs_txn_begin_cursor(p->pSrcTxn, 0, 0, &pZc, 0);
  if( zrc!=ZS_OK ) return zsbkErr(zrc);
  while( zs_cursor_next(pZc, &k, &nk, &v, &nv)==ZS_OK ) n++;
  zs_cursor_fini(&pZc);
  p->nPagecount = p->nRemaining = n;

  rc = zsbkClearDest(p);
  if( rc!=SQLITE_OK ) return rc;

  zrc = zs_txn_begin_cursor(p->pSrcTxn, 0, 0, &p->pCur, 0);
  return zsbkErr(zrc);
}

sqlite3_backup *sqlite3_backup_init(
  sqlite3* pDestDb,
  const char *zDestName,
  sqlite3* pSrcDb,
  const char *zSrcName
){
  sqlite3_backup *p = 0;

#ifdef SQLITE_ENABLE_API_ARMOR
  if( !sqlite3SafetyCheckOk(pSrcDb)||!sqlite3SafetyCheckOk(pDestDb) ){
    (void)SQLITE_MISUSE_BKPT;
    return 0;
  }
#endif
  sqlite3_mutex_enter(pSrcDb->mutex);
  sqlite3_mutex_enter(pDestDb->mutex);

  if( pSrcDb==pDestDb ){
    sqlite3ErrorWithMsg(pDestDb, SQLITE_ERROR,
                        "source and destination must be distinct");
  }else{
    p = (sqlite3_backup*)sqlite3MallocZero(sizeof(sqlite3_backup));
    if( p==0 ){
      sqlite3Error(pDestDb, SQLITE_NOMEM_BKPT);
    }
  }
  if( p ){
    p->pDest = zsbkFindBtree(pDestDb, pDestDb, zDestName);
    p->pSrc = zsbkFindBtree(pDestDb, pSrcDb, zSrcName);
    p->pDestDb = pDestDb;
    p->pSrcDb = pSrcDb;
    if( p->pDest==0 || p->pSrc==0 ){
      sqlite3_free(p);
      p = 0;
    }else if( sqlite3BtreeTxnState(p->pDest)!=SQLITE_TXN_NONE ){
      sqlite3ErrorWithMsg(pDestDb, SQLITE_ERROR,
                          "destination database is in use");
      sqlite3_free(p);
      p = 0;
    }else{
      p->rc = SQLITE_OK;
      sqlite3ZsBtreeSetInBackup(p->pDest, 1);
    }
  }

  sqlite3_mutex_leave(pDestDb->mutex);
  sqlite3_mutex_leave(pSrcDb->mutex);
  return p;
}

int sqlite3_backup_step(sqlite3_backup *p, int nPage){
  const char *k, *v;
  size_t nk, nv;
  int nCopy;
  int zrc = ZS_OK;

#ifdef SQLITE_ENABLE_API_ARMOR
  if( p==0 ) return SQLITE_MISUSE_BKPT;
#endif
  sqlite3_mutex_enter(p->pSrcDb->mutex);
  sqlite3_mutex_enter(p->pDestDb->mutex);

  if( p->rc==SQLITE_OK && p->pSrcTxn==0 ){
    p->rc = zsbkBegin(p);
  }
  if( p->rc==SQLITE_OK ){
    /* "pages" are a fiction here; scale to a record batch */
    nCopy = nPage<0 ? -1 : (nPage>0 ? nPage : 1)*16;
    while( nCopy!=0 ){
      zrc = zs_cursor_next(p->pCur, &k, &nk, &v, &nv);
      if( zrc!=ZS_OK ) break;
      zrc = zs_txn_store(p->pDestTxn, k, nk, v, nv, 0);
      if( zrc!=ZS_OK ) break;
      if( p->nRemaining>0 ) p->nRemaining--;
      if( nCopy>0 ) nCopy--;
    }
    if( zrc==ZS_DONE || zrc==ZS_NOTFOUND ){
      p->rc = SQLITE_DONE;
      p->nRemaining = 0;
    }else if( zrc!=ZS_OK ){
      p->rc = zsbkErr(zrc);
    }
  }

  sqlite3_mutex_leave(p->pDestDb->mutex);
  sqlite3_mutex_leave(p->pSrcDb->mutex);
  return p->rc;
}

int sqlite3_backup_finish(sqlite3_backup *p){
  int rc;

  if( p==0 ) return SQLITE_OK;
  sqlite3_mutex_enter(p->pSrcDb->mutex);
  sqlite3_mutex_enter(p->pDestDb->mutex);

  if( p->pCur ) zs_cursor_fini(&p->pCur);
  if( p->pDestTxn ){
    if( p->rc==SQLITE_DONE ){
      int zrc = zs_txn_commit(&p->pDestTxn);
      if( zrc!=ZS_OK ) p->rc = zsbkErr(zrc);
    }else{
      /* incomplete: the destination keeps its old content */
      zs_txn_abort(&p->pDestTxn);
    }
  }
  if( p->pSrcTxn ) zs_txn_abort(&p->pSrcTxn);
  if( p->rc==SQLITE_DONE ){
    struct zs_db *pZs = sqlite3ZsBtreeDb(p->pDest);
    if( pZs ) zs_db_compact(pZs);   /* fresh content: merge to one file */
  }
  sqlite3ZsBtreeInvalidate(p->pDest);
  sqlite3ZsBtreeSetInBackup(p->pDest, 0);

  rc = (p->rc==SQLITE_DONE) ? SQLITE_OK : p->rc;
  sqlite3Error(p->pDestDb, rc);

  sqlite3_mutex_leave(p->pDestDb->mutex);
  sqlite3_mutex_leave(p->pSrcDb->mutex);
  sqlite3_free(p);
  return rc;
}

int sqlite3_backup_remaining(sqlite3_backup *p){
#ifdef SQLITE_ENABLE_API_ARMOR
  if( p==0 ) return 0;
#endif
  return p->nRemaining;
}

int sqlite3_backup_pagecount(sqlite3_backup *p){
#ifdef SQLITE_ENABLE_API_ARMOR
  if( p==0 ) return 0;
#endif
  return p->nPagecount;
}

/*
** Copy pFrom's entire content into pTo.  Called by VACUUM with write
** transactions already open on both btrees, so the copy reuses them.
*/
int sqlite3BtreeCopyFile(Btree *pTo, Btree *pFrom){
  struct zs_txn *pToTxn = sqlite3ZsBtreeTxn(pTo);
  struct zs_txn *pFromTxn = sqlite3ZsBtreeTxn(pFrom);
  struct zs_cursor *pZc = 0;
  const char *k, *v;
  size_t nk, nv;
  int zrc;

  if( pToTxn==0 || pFromTxn==0 ) return SQLITE_INTERNAL;

  /* clear the destination */
  zrc = zs_txn_begin_cursor(pToTxn, 0, 0, &pZc, 0);
  if( zrc!=ZS_OK ) return zsbkErr(zrc);
  while( (zrc = zs_cursor_next(pZc, &k, &nk, &v, &nv))==ZS_OK ){
    zrc = zs_txn_store(pToTxn, k, nk, 0, 0, 0);
    if( zrc!=ZS_OK ) break;
  }
  zs_cursor_fini(&pZc);
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbkErr(zrc);

  /* copy everything, meta and tree counter included */
  zrc = zs_txn_begin_cursor(pFromTxn, 0, 0, &pZc, 0);
  if( zrc!=ZS_OK ) return zsbkErr(zrc);
  while( (zrc = zs_cursor_next(pZc, &k, &nk, &v, &nv))==ZS_OK ){
    zrc = zs_txn_store(pToTxn, k, nk, v, nv, 0);
    if( zrc!=ZS_OK ) break;
  }
  zs_cursor_fini(&pZc);
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbkErr(zrc);

  sqlite3ZsBtreeInvalidate(pTo);
  /* VACUUM: merge to a single file once this write txn commits */
  sqlite3ZsBtreeScheduleCompact(pTo);
  return SQLITE_OK;
}

/*
** Internal hooks called by the pager when pages change under an
** attached backup.  Zeroskip backups read a snapshot and never attach
** to a pager, so these are no-ops.
*/
void sqlite3BackupRestart(sqlite3_backup *pBackup){
  (void)pBackup;
}
void sqlite3BackupUpdate(sqlite3_backup *pBackup, Pgno iPage, const u8 *aData){
  (void)pBackup; (void)iPage; (void)aData;
}

#endif /* SQLITE_ZEROSKIP */
