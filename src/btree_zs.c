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
** Zeroskip storage engine: implements the btree.h interface on top of
** the zeroskip append-only key-value store (ext/zeroskip).  Compiled in
** place of btree.c when SQLITE_ZEROSKIP is defined.
**
** Key-space layout: every zeroskip key is [4-byte BE tree-id][payload].
** Tree-id 0 is meta: [0,0,0,0,'M',slot] for the 16 meta slots,
** [0,0,0,0,'T'] for the tree-id allocation counter.
** Tree-id 1 is sqlite_schema, matching SQLite's root-page convention.
**
** Design: docs/superpowers/specs/2026-08-11-zeroskip-btree-engine-design.md
*/
#ifdef SQLITE_ZEROSKIP
#ifndef SQLITE_OMIT_SHARED_CACHE
# error "SQLITE_ZEROSKIP requires SQLITE_OMIT_SHARED_CACHE"
#endif
#include "sqliteInt.h"
#include "btree.h"
#include "pager.h"
#include "vdbeInt.h"
#include "zeroskip.h"
#include "zskey.h"
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>

/* Transaction states; values match SQLITE_TXN_NONE/READ/WRITE so that
** sqlite3BtreeTxnState() can return eTxn directly. */
#define ZS_TRANS_NONE  0
#define ZS_TRANS_READ  1
#define ZS_TRANS_WRITE 2

/*
** Savepoint undo log.  Zeroskip transactions are flat, so nested
** savepoints and statement journals are emulated above them: while any
** savepoint mark is open, every write records the key's before-image
** first, and ROLLBACK TO replays before-images in reverse.
**
** Before-images are BORROWED, not copied.  A-4 promises pointers stay
** valid for the transaction's whole life, and since A-4a a snapshot
** swap transfers its reference into the hold list rather than dropping
** it -- so a borrow survives the cursor close that used to unmap it
** (a fetch, then a cursor open across the transaction's first store,
** is the shape a savepoint workload hits).
**
** ZS_NOTFOUND, not a NULL pointer, is what says "the key was absent":
** a zero-length value comes back non-NULL with nOld==0 (A-1).
*/
typedef struct ZsUndoEntry ZsUndoEntry;
struct ZsUndoEntry {
  u8 *aKey;                     /* sqlite3_malloc'd copy, with tree prefix */
  int nKey;
  const char *pOld;             /* borrowed before-image, 0 = absent */
  size_t nOld;
};
typedef struct ZsUndo ZsUndo;
struct ZsUndo {
  ZsUndoEntry *a;               /* entries in write order */
  int n;
  int nAlloc;
  int *aMark;                   /* aMark[i] = undo length when savepoint
                                ** i was opened */
  int nMark;                    /* number of open savepoint marks */
  int nMarkAlloc;
};

/*
** Per-tree append bound, valid only within the current WRITE transaction:
** seeded from the tree's actual largest key (which saw the whole
** snapshot) and raised by our own inserts.  A valid bound NAMES A KEY
** THAT IS IN THE TREE and no key in the tree is above it, so it serves
** both as "cannot exist above this" and as a cursor position that costs
** no fetch.  See zsbtMaxEnsure for how that exactness is maintained and
** which paths are responsible for it.
*/
#define ZS_NMAXCACHE 8
typedef struct ZsTreeMax ZsTreeMax;
struct ZsTreeMax {
  u32 iTree;
  u8 valid;
  ZsKeyBuf max;
};

struct Btree {
  sqlite3 *db;                  /* database connection holding this btree */
  struct zs_db *pZs;            /* zeroskip handle, 0 until first use */
  struct zs_txn *pTxn;          /* open txn or 0 */
  int eTxn;                     /* ZS_TRANS_NONE, _READ or _WRITE */
  char *zDir;                   /* database directory (malloc'd) */
  u8 isEphemeral;               /* delete zDir recursively on close */
  u8 isReadonly;                /* opened with SQLITE_OPEN_READONLY */
  u8 inBackup;                  /* sqlite3BtreeIsInBackup */
  u8 uriNoSync;                 /* URI zs_nosync=1: open with ZS_NOSYNC */
  /* There was a uriNoCsum here (URI zs_nocsum=1 -> ZS_NOCSUM).  Format 3
  ** removed the flag: no record carries a checksum (F-13a) and an in-order
  ** file's regions are never verified on a read path (F-33a), so there is
  ** nothing left to switch off.  The library REJECTS the bit rather than
  ** ignoring it (A-18), so the parameter had to go rather than become inert.
  ** The trade it used to buy is now unconditional -- see the doc. */
  sqlite3_int64 uriRollover;    /* URI zs_rollover=BYTES: generation size, 0 =
                                ** the library's 2MB default.  Bytes only --
                                ** see zsbtEnsureOpen for why the span bound
                                ** is left where it is */
  /* There was a uriMergeMemory here (URI zs_merge_memory=BYTES ->
  ** setup.merge_memory).  It bounded what an in-order output could hold,
  ** because format 3 built the output in memory before it could checksum and
  ** write it, and a compaction's output is O(DATABASE).  Format 4 puts the
  ** pointer array AFTER the records, so a merge holds 8 bytes per record in a
  ** single pass and there is no output to bound; upstream removed the field
  ** with the format.  See the doc: the ceiling is gone, not defaulted. */
  sqlite3_int64 uriRolloverTxns; /* URI zs_rollover_txns=N: the replay window in
                                ** SPANS.  Exists for FIXTURES, which must pin
                                ** any bound they depend on -- upstream's own
                                ** open-cost bench measured a 34x cache win
                                ** that was really the cache moving this bound
                                ** and so preventing the file from sealing.
                                ** Raising it in production removes the
                                ** protection described at the setup below. */
  u8 uriNoRepack;               /* URI zs_norepack=1: open ZS_NOAUTOREPACK, so
                                ** the cascade never runs from a write txn and
                                ** the caller drives it from idle time
                                ** (sqlite3ZsRepackCatchUp) */
  u8 uriCsumNone;               /* URI zs_csum=none: write engine-0 files */
  u8 uriIndexLocal;             /* URI zs_index=local: pointer-table cache */
  char *zIndexDir;              /* URI zs_index_dir=PATH, else 0 (owned) */
  u8 wantCompact;               /* run zs_db_compact after the next commit */
  int syncMs;                   /* URI zs_sync_ms=N: post-commit sync cadence */
  sqlite3_int64 lastSyncMs;     /* last periodic zs_db_sync */
  Pager *pFakePager;            /* vestigial in-memory pager */
  Schema *pSchema;              /* schema object, owned */
  void (*xFreeSchema)(void*);   /* destructor for pSchema */
  BtCursor *pCursor;            /* list of open cursors */
  u64 writeEpoch;               /* bumped on every write; cursors re-seek */
  ZsUndo undo;                  /* savepoint undo log */
  ZsTreeMax aMax[ZS_NMAXCACHE]; /* per-tree append bounds (write txn only) */
  u32 iDataVersion;             /* BTREE_DATA_VERSION surrogate */
  /* Meta values (the schema cookie above all) are read at the start of
  ** EVERY statement, and each read was a full key lookup through the
  ** merge machinery where stock reads a cached page-1 header.  The
  ** transaction's snapshot is fixed, and sqlite3BtreeUpdateMeta is the
  ** only writer inside it, so the values can be cached for the life of
  ** the transaction and written through on update.  pMetaTxn scopes the
  ** cache: it is cleared whenever a transaction is created. */
  struct zs_txn *pMetaTxn;      /* txn aMetaVal belongs to, 0 = none */
  u16 metaValid;                /* bit idx set = aMetaVal[idx] is live */
  u32 aMetaVal[16];
};

/* Where a cursor's pVal came from, which is what decides how long it lives.
** A-4 gives a borrow the lifetime of whatever produced it: a fetch borrow
** lasts the whole transaction, a cursor borrow only as long as its cursor --
** and this engine finalises cursors on insert, delete and re-seek.
*/
#define ZS_VAL_CURSOR  0        /* a cursor step: dies with pCur->pZc */
#define ZS_VAL_ABSENT  1        /* not kept (ephemeral probe): re-read to use */
#define ZS_VAL_FETCH   2        /* a fetch: valid for the whole transaction */

/* Cursor states.  eState MUST be the first member of BtCursor and
** ZS_CUR_VALID must be usable from a one-byte static, so that
** sqlite3BtreeFakeValidCursor() can work the same way as stock. */
#define ZS_CUR_INVALID     0    /* no position */
#define ZS_CUR_VALID       1    /* borrowed position is current */
#define ZS_CUR_REQUIRESEEK 2    /* position saved in aSavedKey, re-seek */
#define ZS_CUR_FAULT       3    /* return skipNext as error code */

/* LAYOUT CONSTRAINT: exactly EIGHT u8 fields may precede pBtree.
**
** test3.c's btree_* TCL commands reach into BtCursor through btreeInt.h's
** STOCK definition, where four u8s and an int put pBtree at offset 8.  The
** eight u8s below land it in the same place, and that coincidence is the only
** reason those commands do not read a wild pointer.  A NINTH u8 pushes pBtree
** to 16 and segfaults fordelete.test and types.test -- which is how this was
** found, after the TCL permutation had been clean for months.  Pack a new
** flag into an existing byte, as valSrc does, rather than adding one.
*/
struct BtCursor {
  u8 eState;                    /* MUST be first (FakeValidCursor) */
  u8 curIntKey;                 /* true for BTREE_INTKEY trees */
  u8 wrFlag;
  u8 isReverse;                 /* underlying zs cursor runs backwards */
  u8 isPinned;                  /* OP_CursorLock: writes moving us fail */
  u8 skipNextAdv;               /* restore landed on the successor: the
                                ** next Next() must not advance */
  u8 valSrc;                    /* where pVal came from, and so how long it
                                ** lives: ZS_VAL_*.  One field rather than two
                                ** booleans because of the layout constraint
                                ** noted above this struct */
  u8 skipPrevAdv;               /* restore landed on the predecessor (the
                                ** vanished row had no successor): the next
                                ** Previous() must not step back */
  Btree *pBtree;
  u32 iTree;
  u8 aPrefix[4];                /* big-endian iTree, the key prefix */
  struct zs_cursor *pZc;        /* open zeroskip cursor or 0 */
  unsigned int hints;
  int skipNext;                 /* error code when eState==ZS_CUR_FAULT */
  u64 seenEpoch;                /* writeEpoch when position was taken */
  KeyInfo *pKeyInfo;            /* for index trees, else 0 */
  const char *pKey;             /* borrowed current key (incl. prefix) */
  size_t nKey;
  const char *pVal;             /* borrowed current value */
  size_t nVal;
  i64 intKey;                   /* decoded rowid when curIntKey */
  u8 *aSavedKey;                /* heap copy of position (trip/upgrade) */
  int nSavedKey;
  ZsKeyBuf encBuf;              /* reusable key-encode buffer */
  UnpackedRecord *pUnpacked;    /* reusable unpack scratch for pKeyInfo */
  BtCursor *pNext;              /* next cursor on the same Btree */
};

static int zsbtSavePosition(BtCursor *pCur, const char *aKey, size_t nKey);
static int zsbtKeyCmp(const char *a, size_t na, const char *b, size_t nb);
static int zsbtPointLE(BtCursor *pCur, const u8 *aKey, size_t nKey);
static int zsbtPointFetch(BtCursor *pCur, const u8 *aKey, size_t nKey,
                          int dirFlag, int bProbe);
static int zsbtLoadValue(BtCursor *pCur);
static int zsbtCanProbe(BtCursor *pCur);
static int zsbtCheckPinned(Btree *p, u32 iTree, BtCursor *pExcept);

/*
** Map a zeroskip return code to an SQLite error code.  ZS_NOTFOUND is
** deliberately absent: it is a result, not an error, and every call
** site handles it before coming here.
*/
static int zsbtErr(int zsrc){
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

/*************************************************************************
** Key-space helpers
*/

/* Meta keys: [0,0,0,0,'M',idx].  Returns the key length. */
static int zsbtMetaKey(u8 *a, int idx){
  memset(a, 0, 4);
  a[4] = 'M';
  a[5] = (u8)idx;
  return 6;
}

/* Tree-id allocation counter key: [0,0,0,0,'T'] */
static int zsbtCounterKey(u8 *a){
  memset(a, 0, 4);
  a[4] = 'T';
  return 5;
}

/* Find (or, if addIfAbsent, claim) the append-bound slot for a tree. */
static ZsTreeMax *zsbtMaxSlot(Btree *p, u32 iTree, int addIfAbsent){
  int i;
  for(i=0; i<ZS_NMAXCACHE; i++){
    if( p->aMax[i].valid && p->aMax[i].iTree==iTree ) return &p->aMax[i];
  }
  if( addIfAbsent ){
    for(i=0; i<ZS_NMAXCACHE; i++){
      if( !p->aMax[i].valid ){
        p->aMax[i].iTree = iTree;
        return &p->aMax[i];
      }
    }
  }
  return 0;
}

static void zsbtMaxInvalidate(Btree *p){
  int i;
  for(i=0; i<ZS_NMAXCACHE; i++) p->aMax[i].valid = 0;
}

/* Raise a bound to aKey if aKey is larger.  Takes the slot rather than
** finding it, because its only caller is the write funnel, which already
** has it in hand. */
static void zsbtMaxRaise(ZsTreeMax *pM, const u8 *aKey, int nKey){
  int c;
  assert( pM!=0 && pM->valid );
  c = memcmp(aKey, pM->max.a, nKey<pM->max.n ? nKey : pM->max.n);
  if( c>0 || (c==0 && nKey>pM->max.n) ){
    pM->max.n = 0;
    if( zskeyBufAppend(&pM->max, aKey, nKey)!=SQLITE_OK ) pM->valid = 0;
  }
}

/* The before-image the undo log needs, taken from a cursor that is already
** sitting on the row about to be overwritten.  An UPDATE or DELETE by
** primary key has just seeked that row and read it, so fetching it again
** repeats a lookup this layer already paid for -- worth 17% of a
** point-update statement.  Returns 0 when the cursor cannot answer, and
** then the caller fetches as before.
**
** Every condition is load-bearing:
**
**   pCur         a caller with no cursor (meta writes, the prefix-equal
**                probe delete) passes 0;
**   VALID+epoch  the payload must describe the CURRENT version -- a write
**                to this key since the cursor loaded it bumps the epoch;
**   ZS_VAL_FETCH A-4 gives a fetch borrow the TRANSACTION's lifetime and a
**                cursor borrow only its CURSOR's, and this engine finalises
**                cursors on insert, delete and re-seek, so an undo entry
**                outlives it.  ZS_VAL_ABSENT means an ephemeral probe kept
**                the position and dropped the value: nothing to take;
**   same key     a cursor may sit anywhere.  OP_NewRowid leaves it on the
**                largest key while the insert goes to a new one, so this is
**                not a theoretical case.
*/
static const char *zsbtUndoHint(
  BtCursor *pCur,
  const u8 *aKey, size_t nKey,
  size_t *pnOld
){
  if( pCur==0 ) return 0;
  if( pCur->eState!=ZS_CUR_VALID ) return 0;
  if( pCur->valSrc!=ZS_VAL_FETCH || pCur->pVal==0 ) return 0;
  if( pCur->seenEpoch!=pCur->pBtree->writeEpoch ) return 0;
  if( (size_t)pCur->nSavedKey!=nKey
   || memcmp(pCur->aSavedKey, aKey, nKey)!=0 ) return 0;
  *pnOld = pCur->nVal;
  return pCur->pVal;
}

/*************************************************************************
** Write funnel.  EVERY mutation of the zeroskip database goes through
** here (the savepoint undo hook and the write epoch depend on it).
** pVal==0 is a delete.  pHint, when given, is a cursor that may already
** hold this key's before-image -- see zsbtUndoHint.
*/
static int zsbtWrite(
  Btree *p,
  const u8 *aKey, size_t nKey,
  const char *pVal, size_t nVal,
  BtCursor *pHint
){
  int zrc;
  /* The tree's append bound, if this transaction has one.  Looked up once:
  ** the undo shortcut below reads it, and the maintenance after the store
  ** both raises and invalidates it.  Never seeded here -- seeding costs the
  ** fetch this is avoiding, and zsbtMaxSlot with addIfAbsent 0 only returns
  ** a bound that zsbtMaxEnsure already seeded from the tree's true max. */
  ZsTreeMax *pM;
  assert( p->eTxn==ZS_TRANS_WRITE && p->pTxn!=0 );
  pM = nKey>=4 ? zsbtMaxSlot(p, zskeyGetTreeId(aKey), 0) : 0;
  /* A transaction savepoint (autocommit SAVEPOINT) is not counted in
  ** db->nSavepoint and its rollback arrives as iSavepoint==-1, meaning
  ** "undo the whole transaction but keep it open" -- so the undo log
  ** must cover the transaction from its first write. */
  if( p->undo.nMark>0 || p->db->isTransactionSavepoint ){
    ZsUndo *u = &p->undo;
    ZsUndoEntry *pE;
    const char *pOld;
    size_t nOld;
    /* The bound can answer this without a lookup.  A valid bound NAMES a
    ** live key with nothing above it (zsbtMaxEnsure), so a key STRICTLY
    ** above it has no current version and there is nothing to save --
    ** which is the whole of a bulk INSERT, where this fetch otherwise
    ** misses once per row and costs 17% of the statement.
    **
    ** STRICTLY: a key EQUAL to the bound is present, and recording it as
    ** absent makes a rollback DELETE that row instead of restoring it.
    **
    ** The raise below is what makes this sound, and it lives HERE rather
    ** than in sqlite3BtreeInsert (which also raises, harmlessly) because
    ** the funnel must maintain what the funnel consumes: a write path that
    ** inserted above the bound without raising it would leave a later
    ** overwrite of that same key looking absent, and a ROLLBACK TO between
    ** the two writes would delete the row rather than restore the first
    ** version.  Keeping both halves in one place is what keeps that
    ** unreachable.  test/zs/11-undoskip section 3 is that case. */
    if( pM && zsbtKeyCmp((const char*)aKey, nKey,
                         (const char*)pM->max.a, (size_t)pM->max.n)>0 ){
      zrc = ZS_NOTFOUND;
      pOld = 0;
      nOld = 0;
    }else if( (pOld = zsbtUndoHint(pHint, aKey, nKey, &nOld))!=0 ){
      zrc = ZS_OK;
    }else{
      zrc = zs_txn_fetch(p->pTxn, (const char*)aKey, nKey, 0, 0,
                         &pOld, &nOld, 0);
      if( zrc!=ZS_OK && zrc!=ZS_NOTFOUND ) return zsbtErr(zrc);
    }
    if( u->n>=u->nAlloc ){
      int nNew = u->nAlloc ? u->nAlloc*2 : 64;
      ZsUndoEntry *aNew = sqlite3Realloc(u->a, nNew*sizeof(*aNew));
      if( aNew==0 ) return SQLITE_NOMEM_BKPT;
      u->a = aNew;
      u->nAlloc = nNew;
    }
    pE = &u->a[u->n];
    pE->aKey = sqlite3_malloc64(nKey ? nKey : 1);
    if( pE->aKey==0 ) return SQLITE_NOMEM_BKPT;
    memcpy(pE->aKey, aKey, nKey);
    pE->nKey = (int)nKey;
    if( zrc==ZS_OK ){
      pE->pOld = pOld;          /* borrow: valid for the whole txn (A-4) */
      pE->nOld = nOld;
    }else{
      pE->pOld = 0;
      pE->nOld = 0;
    }
    u->n++;
  }
  zrc = zs_txn_store(p->pTxn, (const char*)aKey, nKey, pVal, nVal, 0);
  if( zrc!=ZS_OK ) return zsbtErr(zrc);
  /* Keep the append bound exact: a delete that removes the key it names
  ** (or anything above it, which cannot exist, but costs nothing to
  ** allow) leaves it naming a row that is gone, and it is used as a
  ** POSITION.  Every delete in the engine arrives here -- that is what
  ** this funnel is for -- including ClearTable's key-by-key sweep. */
  if( pM ){
    if( pVal==0 ){
      if( zsbtKeyCmp((const char*)aKey, nKey,
                     (const char*)pM->max.a, (size_t)pM->max.n)>=0 ){
        pM->valid = 0;
      }
    }else{
      /* An insert raises the bound, which is what lets the undo shortcut
      ** above treat "above the bound" as "absent": this key is now present,
      ** so writing it again must find it.  This is the ONLY place a bound is
      ** raised -- sqlite3BtreeInsert used to do it too, redundantly, and
      ** moving it here is what makes the shortcut's invariant local to the
      ** funnel instead of spread across the callers that feed it. */
      zsbtMaxRaise(pM, aKey, (int)nKey);
    }
  }
  p->writeEpoch++;
  return SQLITE_OK;
}

/* Free all undo entries and marks (transaction end). */
static void zsbtUndoReset(Btree *p){
  ZsUndo *u = &p->undo;
  int i;
  for(i=0; i<u->n; i++){
    sqlite3_free(u->a[i].aKey);
  }
  u->n = 0;
  u->nMark = 0;
}

/* Replay before-images in reverse down to undo length iTo. */
static int zsbtUndoReplay(Btree *p, int iTo){
  ZsUndo *u = &p->undo;
  int i;
  /* replay can remove keys, but the stale-high bounds stay safe;
  ** invalidate anyway to keep reasoning simple */
  zsbtMaxInvalidate(p);
  /* A replayed before-image can restore a meta key (a DDL statement
  ** rolled back to a savepoint), and it goes straight to zs_txn_store
  ** rather than through sqlite3BtreeUpdateMeta, so the cache cannot be
  ** written through here. */
  p->metaValid = 0;
  for(i=u->n-1; i>=iTo; i--){
    ZsUndoEntry *pE = &u->a[i];
    int zrc = zs_txn_store(p->pTxn, (const char*)pE->aKey, pE->nKey,
                           pE->pOld, pE->pOld ? pE->nOld : 0, 0);
    if( zrc!=ZS_OK ) return zsbtErr(zrc);
    sqlite3_free(pE->aKey);
    u->n = i;
  }
  p->writeEpoch++;
  return SQLITE_OK;
}

/*************************************************************************
** Btree object lifecycle
*/

static void zsbtPageReinit(DbPage *pPage){
  (void)pPage;
}

/* Recursively delete a (flat) ephemeral database directory. */
static void zsbtRemoveDir(const char *zDir){
  DIR *d = opendir(zDir);
  if( d ){
    struct dirent *pEnt;
    while( (pEnt = readdir(d))!=0 ){
      char *zPath;
      if( strcmp(pEnt->d_name,".")==0 || strcmp(pEnt->d_name,"..")==0 ){
        continue;
      }
      zPath = sqlite3_mprintf("%s/%s", zDir, pEnt->d_name);
      if( zPath ){
        if( unlink(zPath)!=0 ) zsbtRemoveDir(zPath);  /* subdir: recurse */
        sqlite3_free(zPath);
      }
    }
    closedir(d);
  }
  rmdir(zDir);
}

/* Open the zeroskip handle if it is not open yet.  Deferred from
** sqlite3BtreeOpen because SQLite opens btrees it may never use. */
static int zsbtEnsureOpen(Btree *p){
  struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
  int zrc;
  if( p->pZs ) return SQLITE_OK;
  setup.flags = ZS_NONBLOCKING;
  if( p->isReadonly ){
    setup.flags |= ZS_SHARED;
  }else{
    setup.flags |= ZS_CREATE;
  }
  /* The library's own cascade (D-16e) is left ARMED.  It runs at BEGIN,
  ** where nothing is held yet, and only when the transaction is about to
  ** start a new generation, which is the only way the file count grows.
  **
  ** The reason recorded here used to be "C-1d orders repack before write, so
  ** the merge cannot hold the write lock".  That died when C-1d reversed to
  ** write -> repack: a commit CAN now take the repack lock in order, and
  ** C-1l's compacting seal does exactly that.  The real reason is that the
  ** cascade is UNBOUNDED (D-16b) -- taking repack from inside a commit would
  ** hold the WRITE lock across an unbounded merge and block every other
  ** writer for its whole duration.  At BEGIN nothing is held, so the merge
  ** runs under the repack lock alone.  That trigger is narrower and better placed than the
  ** engine's old post-commit "repack whenever two files could merge",
  ** which fired on nearly every commit and spent write throughput to buy
  ** a read latency the workload had not asked for. */
  if( p->isEphemeral || p->uriNoSync ) setup.flags |= ZS_NOSYNC;
  /* zs_norepack takes the cascade OFF the write path entirely.  Not a
  ** default and not a mode to leave on: the file count then grows without
  ** limit until something calls the catch-up, and point-lookup cost is
  ** linear in it (D-14d).  It exists because deferring the cascade does not
  ** just move the work, it REMOVES most of it -- the ladder re-merges the
  ** same bytes about three times on the way up, so one merge afterwards
  ** rewrites a third of what the cascade rewrote and ends with fewer files.
  ** That makes it a bulk-load-window shape: a restore or a migration, with
  ** the catch-up run before readers come back. */
  if( p->uriNoRepack ) setup.flags |= ZS_NOAUTOREPACK;
  /* A larger generation attacks BOTH per-file axes at once: fewer files, so
  ** fewer lifecycle events, and a shallower merge ladder, so fewer bytes
  ** re-merged on the way up.  Upstream measures 2MB -> 16MB taking a 2M-record
  ** load from 269 unlinks and 796MB of merging to 33 and 449MB.
  **
  ** It is NOT monotonic and must be measured rather than raised on principle:
  ** past some size the active file's private index, and the D-13b delta flush
  ** that is linear in it, cost more than the churn they save -- 64MB is slower
  ** than the 2MB default on upstream's laptop.
  **
  ** And it moves cost onto SNAPSHOT OPEN, which is linear in the active file's
  ** spans: this document's own table has plain open going 0.06ms at 33KB to
  ** 3.4ms at 2MB.  A deployment whose writers are separate short-lived
  ** processes -- which is what this engine is for -- pays that at every begin,
  ** so a large rollover wants the pointer-table cache (zs_index=local) with
  ** it, not instead of it. */
  if( p->uriRollover>0 ) setup.rollover_size = (size_t)p->uriRollover;
  /* There is no merge_memory to set any more.  Format 3 needed it because an
  ** in-order file's regions had to be sized before the first byte was written,
  ** so a merge accumulated its output; the ceiling existed so that a
  ** zs_db_compact -- which this engine calls from VACUUM and from a backup's
  ** finish, and whose output is O(DATABASE) -- could run on a database larger
  ** than memory.  Format 4's pointer array comes last and its trailer carries
  ** the section offsets, so the output streams and the merge's own memory is
  ** 8 bytes per record.  Whether that actually bounds VACUUM's peak is a
  ** MEASUREMENT we owe: merge_memory never bounded the mapped INPUTS, and
  ** those are what made VACUUM peak at 1.2-1.4GB at 2M records. */
  if( p->uriRolloverTxns>0 ){
    setup.rollover_txns = (size_t)p->uriRolloverTxns;
  }
  /* rollover_txns is deliberately LEFT ALONE.  It bounds the replay window in
  ** SPANS rather than bytes, and it exists (A-15/D-9d) because many small
  ** transactions slip under a byte bound and leave a rebuild that grows
  ** without limit.  Scaling it up with rollover_size -- which this code did
  ** first -- removes that protection from precisely the workload that needs
  ** it: single-record commits, where the replay is paid at every begin by a
  ** short-lived writer process.  Leaving it means a big byte bound simply has
  ** no effect on such a workload, which is the right outcome rather than a
  ** missed opportunity. */
  if( p->uriCsumNone ) setup.flags |= ZS_CSUM_NONE;
  /* Pointer-table cache (spec section 8), OFF by default -- measured, not
  ** assumed.  It bounds snapshot-open cost, which grows with the number
  ** of spans in the active file, and upstream measures up to 43x on that
  ** open.  This engine repacks after every commit that wants it, which
  ** already keeps the active file small, so there is nothing left for the
  ** cache to save: across bulk-loaded, span-heavy, in-process and
  ** cross-process shapes it changed fetches and scans by under 5% while
  ** costing 20-30% on multi-row stores (P-13's write/open trade).
  ** zs_index=local puts tables in zeroskip.cache inside the database
  ** directory; zs_index_dir=PATH puts them elsewhere (mutually exclusive,
  ** A-8a).  Worth enabling for a workload that opens far more often than
  ** it writes. */
  if( p->zIndexDir ){
    setup.index_dir = p->zIndexDir;
  }else if( p->uriIndexLocal ){
    setup.flags |= ZS_INDEX_LOCAL;
  }
  zrc = zs_db_open(p->zDir, &setup, &p->pZs);
  if( zrc!=ZS_OK && !p->isReadonly && !p->isEphemeral ){
    /* An unwritable existing database degrades to a read-only open,
    ** as stock does; writes then fail with SQLITE_READONLY. */
    setup.flags = (setup.flags & ~(uint32_t)ZS_CREATE) | ZS_SHARED;
    if( zs_db_open(p->zDir, &setup, &p->pZs)==ZS_OK ){
      p->isReadonly = 1;
      return SQLITE_OK;
    }
  }
  if( zrc==ZS_NOTFOUND ) return SQLITE_CANTOPEN_BKPT;
  if( zrc!=ZS_OK ){
    /* A zeroskip database is a directory.  When the path exists and is
    ** something else -- typically a file that is not a database at all
    ** (misc5-4.1) -- report it the way stock reports a bad header
    ** rather than as an I/O failure. */
    struct stat sb;
    if( stat(p->zDir, &sb)==0 && !S_ISDIR(sb.st_mode) ){
      return SQLITE_NOTADB;
    }
  }
  return zsbtErr(zrc);
}

int sqlite3BtreeOpen(
  sqlite3_vfs *pVfs,
  const char *zFilename,
  sqlite3 *db,
  Btree **ppBtree,
  int flags,
  int vfsFlags
){
  Btree *p;
  int rc;
  int isEphemeral;

  (void)flags;
  *ppBtree = 0;
  isEphemeral = zFilename==0 || zFilename[0]==0
             || strcmp(zFilename, ":memory:")==0;

  p = sqlite3MallocZero(sizeof(*p));
  if( p==0 ) return SQLITE_NOMEM_BKPT;
  p->db = db;
  p->eTxn = ZS_TRANS_NONE;
  p->iDataVersion = 1;

  if( isEphemeral ){
    const char *zTmp = sqlite3_temp_directory;
    if( zTmp==0 ) zTmp = "/tmp";
    p->zDir = sqlite3_mprintf("%s/zs-ephem-XXXXXX", zTmp);
    if( p->zDir==0 ){
      sqlite3_free(p);
      return SQLITE_NOMEM_BKPT;
    }
    if( mkdtemp(p->zDir)==0 ){
      sqlite3_free(p->zDir);
      sqlite3_free(p);
      return SQLITE_CANTOPEN_BKPT;
    }
    p->isEphemeral = 1;
  }else{
    p->zDir = sqlite3_mprintf("%s", zFilename);
    if( p->zDir==0 ){
      sqlite3_free(p);
      return SQLITE_NOMEM_BKPT;
    }
    p->isReadonly = (vfsFlags & SQLITE_OPEN_READONLY)!=0;
    /* Durability knobs (the WAL/synchronous=NORMAL analog): commits
    ** skip the per-commit gates; a periodic zs_db_sync bounds loss.
    ** sqlite3_uri_* are only defined on names that came through the
    ** URI machinery, hence the flag check. */
    if( (vfsFlags & SQLITE_OPEN_URI)!=0 ){
      const char *zCsum;
      if( sqlite3_uri_boolean(zFilename, "zs_nosync", 0) ){
        p->uriNoSync = 1;
        p->syncMs = (int)sqlite3_uri_int64(zFilename, "zs_sync_ms", 0);
      }
      p->uriNoRepack = (u8)sqlite3_uri_boolean(zFilename, "zs_norepack", 0);
      p->uriRollover = sqlite3_uri_int64(zFilename, "zs_rollover", 0);
      p->uriRolloverTxns = sqlite3_uri_int64(zFilename, "zs_rollover_txns", 0);
      zCsum = sqlite3_uri_parameter(zFilename, "zs_csum");
      if( zCsum && strcmp(zCsum, "none")==0 ) p->uriCsumNone = 1;
      {
        const char *zIdx = sqlite3_uri_parameter(zFilename, "zs_index");
        if( zIdx && strcmp(zIdx, "local")==0 ) p->uriIndexLocal = 1;
        zIdx = sqlite3_uri_parameter(zFilename, "zs_index_dir");
        /* copied: the open is deferred past the caller's URI string */
        if( zIdx && zIdx[0] ) p->zIndexDir = sqlite3_mprintf("%s", zIdx);
      }
    }
  }

  /* Vestigial in-memory pager so that sqlite3BtreePager() callers
  ** (pragmas, dbstat, ...) keep functioning.  It stores nothing. */
  if( pVfs==0 ) pVfs = sqlite3_vfs_find(0);
  rc = sqlite3PagerOpen(pVfs, &p->pFakePager, 0, 8,
                        PAGER_MEMORY|PAGER_OMIT_JOURNAL,
                        vfsFlags, zsbtPageReinit);
  if( rc!=SQLITE_OK ){
    if( p->isEphemeral ) zsbtRemoveDir(p->zDir);
    sqlite3_free(p->zDir);
    sqlite3_free(p);
    return rc;
  }

  *ppBtree = p;
  return SQLITE_OK;
}

/* Release the zeroskip cursor (if any) held by pCur. */
static void zsbtCursorFini(BtCursor *pCur){
  if( pCur->pZc ) zs_cursor_fini(&pCur->pZc);
  pCur->pKey = 0;
  pCur->pVal = 0;
}

/* End-of-transaction cleanup shared by commit and rollback paths:
** every zeroskip cursor handle dies with the transaction. */
static void zsbtEndTxn(Btree *p){
  BtCursor *pCur;
  for(pCur=p->pCursor; pCur; pCur=pCur->pNext){
    zsbtCursorFini(pCur);
    if( pCur->eState!=ZS_CUR_FAULT ) pCur->eState = ZS_CUR_INVALID;
  }
  zsbtUndoReset(p);
  zsbtMaxInvalidate(p);
  p->eTxn = ZS_TRANS_NONE;
}

int sqlite3BtreeClose(Btree *p){
  BtCursor *pCur;
  for(pCur=p->pCursor; pCur; pCur=pCur->pNext){
    zsbtCursorFini(pCur);
    pCur->eState = ZS_CUR_INVALID;
  }
  p->pCursor = 0;
  if( p->pTxn ) zs_txn_abort(&p->pTxn);
  zsbtUndoReset(p);
  sqlite3_free(p->undo.a);
  sqlite3_free(p->undo.aMark);
  { int i; for(i=0; i<ZS_NMAXCACHE; i++) sqlite3_free(p->aMax[i].max.a); }
  if( p->pZs ) zs_db_close(&p->pZs);
  sqlite3_free(p->zIndexDir);
  if( p->pFakePager ) sqlite3PagerClose(p->pFakePager, p->db);
  if( p->pSchema ){
    if( p->xFreeSchema ) p->xFreeSchema(p->pSchema);
    sqlite3DbFree(0, p->pSchema);
  }
  if( p->isEphemeral ) zsbtRemoveDir(p->zDir);
  sqlite3_free(p->zDir);
  sqlite3_free(p);
  return SQLITE_OK;
}

/*************************************************************************
** Configuration no-ops (page/cache knobs have no meaning here)
*/

int sqlite3BtreeSetCacheSize(Btree *p, int mxPage){
  (void)p; (void)mxPage; return SQLITE_OK;
}
int sqlite3BtreeSetSpillSize(Btree *p, int mxPage){
  (void)p; (void)mxPage; return 0;
}
#if SQLITE_MAX_MMAP_SIZE>0
int sqlite3BtreeSetMmapLimit(Btree *p, sqlite3_int64 szMmap){
  (void)p; (void)szMmap; return SQLITE_OK;
}
#endif
int sqlite3BtreeSetPagerFlags(Btree *p, unsigned pgFlags){
  (void)p; (void)pgFlags; return SQLITE_OK;
}
int sqlite3BtreeSetPageSize(Btree *p, int pageSize, int nReserve, int eFix){
  /* Accepted and ignored: vacuum.c treats a failure here as OOM. */
  (void)p; (void)pageSize; (void)nReserve; (void)eFix;
  return SQLITE_OK;
}
int sqlite3BtreeGetPageSize(Btree *p){
  (void)p; return 4096;
}
Pgno sqlite3BtreeMaxPageCount(Btree *p, Pgno mxPage){
  (void)p; (void)mxPage; return 0x7fffffff;
}
Pgno sqlite3BtreeLastPage(Btree *p){
  u8 aKey[8];
  int nKey;
  const char *pVal;
  size_t nVal;
  struct zs_txn *pTmp = 0;
  struct zs_txn *pTxn;
  u32 iLast = 1;

  /* The largest allocated tree id; schema loading validates root
  ** numbers against this. */
  if( zsbtEnsureOpen(p)!=SQLITE_OK ) return 1;
  pTxn = p->pTxn;
  if( pTxn==0 ){
    if( zs_db_begin_txn(p->pZs, 1, &pTmp)!=ZS_OK ) return 1;
    pTxn = pTmp;
  }
  nKey = zsbtCounterKey(aKey);
  if( zs_txn_fetch(pTxn, (const char*)aKey, nKey, 0, 0, &pVal, &nVal, 0)==ZS_OK
   && nVal==4 ){
    iLast = zskeyGetTreeId((const u8*)pVal);
  }
  if( pTmp ) zs_txn_abort(&pTmp);
  return (Pgno)iLast;
}
int sqlite3BtreeSecureDelete(Btree *p, int newFlag){
  (void)p; (void)newFlag; return 0;
}
int sqlite3BtreeGetRequestedReserve(Btree *p){
  (void)p; return 0;
}
int sqlite3BtreeGetReserveNoMutex(Btree *p){
  (void)p; return 0;
}
int sqlite3BtreeSetAutoVacuum(Btree *p, int autoVacuum){
  (void)p;
  return autoVacuum==BTREE_AUTOVACUUM_NONE ? SQLITE_OK : SQLITE_READONLY;
}
int sqlite3BtreeGetAutoVacuum(Btree *p){
  (void)p; return BTREE_AUTOVACUUM_NONE;
}

/*************************************************************************
** Transactions
*/

/* Ensure undo marks exist up to nTarget (mirrors stock's lazy
** sqlite3PagerOpenSavepoint usage). */
static int zsbtSavepointTo(Btree *p, int nTarget){
  ZsUndo *u = &p->undo;
  while( u->nMark<nTarget ){
    if( u->nMark>=u->nMarkAlloc ){
      int nNew = u->nMarkAlloc ? u->nMarkAlloc*2 : 16;
      int *aNew = sqlite3Realloc(u->aMark, nNew*sizeof(int));
      if( aNew==0 ) return SQLITE_NOMEM_BKPT;
      u->aMark = aNew;
      u->nMarkAlloc = nNew;
    }
    u->aMark[u->nMark++] = u->n;
  }
  return SQLITE_OK;
}

/*
** Save every positioned cursor's key to the heap and release its
** zeroskip cursor: used before the read->write upgrade (all zs cursors
** die with the read txn) and by TripAllCursors.  A nonzero errCode
** additionally puts cursors into the FAULT state so their next
** operation reports it; with writeOnly only write cursors fault, but
** every cursor's position is saved.
*/
static int zsbtSaveAllCursors(Btree *p, int errCode, int writeOnly){
  BtCursor *pCur;
  for(pCur=p->pCursor; pCur; pCur=pCur->pNext){
    if( pCur->eState==ZS_CUR_VALID ){
      /* aSavedKey already mirrors the position (zsbtLoadCurrent) */
      pCur->eState = ZS_CUR_REQUIRESEEK;
    }
    zsbtCursorFini(pCur);
    if( errCode && (pCur->wrFlag & BTREE_WRCSR || !writeOnly) ){
      pCur->eState = ZS_CUR_FAULT;
      pCur->skipNext = errCode;
    }
  }
  return SQLITE_OK;
}

/* Lock ordering across ATTACHed zeroskip databases (C-1h).
**
** zeroskip orders locks within one database and leaves an order across several
** to the caller, so a connection holding write locks on two attached databases
** could in principle deadlock against one taking them the other way.  What we
** inherit from SQLite makes that tolerable rather than impossible:
**
** - the order is deterministic per connection.  Both places that emit
**   OP_Transaction walk db->aDb in ASCENDING index order (sqlite3FinishCoding
**   and sqlite3BeginTransaction), so within one attach layout every connection
**   locks in the same sequence;
** - two connections whose attach layouts DIFFER (the same two files as
**   main+aux and aux+main) can still take them in opposite orders, and that is
**   a genuine cycle;
** - it resolves by timeout rather than hanging, because the loop below gives
**   up when the busy handler does: SQLITE_BUSY unwinds the statement, SQLite
**   rolls back, and the other side's lock is released.  A caller that installs
**   an INFINITE busy handler removes that escape and can livelock -- which is
**   also true of stock SQLite over two attached files, so it is inherited
**   rather than introduced.
**
** Practical rule for a deployment: attach in the same order everywhere, or set
** sqlite3_busy_timeout rather than an unbounded handler.  Untested here; the
** two-writer coverage is single-database (test/zs/twowriter.test). */
static int zsbtBegin(Btree *p, int wrflag){
  int rc;
  int zrc;

  rc = zsbtEnsureOpen(p);
  if( rc!=SQLITE_OK ) return rc;
  if( wrflag && p->isReadonly ) return SQLITE_READONLY;
  if( wrflag && p->inBackup ) return SQLITE_BUSY;

  if( p->eTxn==ZS_TRANS_NONE ){
    /* The engine owns lock acquisition, so it also owns invoking the
    ** connection's busy handler (the pager does this in stock). */
    while( (zrc = zs_db_begin_txn(p->pZs, wrflag ? 0 : 1, &p->pTxn))==ZS_LOCKED
        && sqlite3InvokeBusyHandler(&p->db->busyHandler) ){}
    if( zrc!=ZS_OK ) return zsbtErr(zrc);
    p->eTxn = wrflag ? ZS_TRANS_WRITE : ZS_TRANS_READ;
    p->pMetaTxn = 0; p->metaValid = 0;
  }else if( wrflag && p->eTxn==ZS_TRANS_READ ){
    /* Read->write upgrade: zeroskip has no in-place upgrade, so abort
    ** the read snapshot and begin a write transaction.  Every zeroskip
    ** cursor dies with the read txn -- the one place positions are
    ** copied to the heap; they re-seek lazily in the new txn. */
    rc = zsbtSaveAllCursors(p, 0, 0);
    if( rc!=SQLITE_OK ) return rc;
    zsbtMaxInvalidate(p);        /* the new snapshot may hold larger keys */
    zs_txn_abort(&p->pTxn);
    p->eTxn = ZS_TRANS_NONE;
    while( (zrc = zs_db_begin_txn(p->pZs, 0, &p->pTxn))==ZS_LOCKED
        && sqlite3InvokeBusyHandler(&p->db->busyHandler) ){}
    if( zrc!=ZS_OK ) return zsbtErr(zrc);
    p->eTxn = ZS_TRANS_WRITE;
    p->pMetaTxn = 0; p->metaValid = 0;
    p->iDataVersion++;          /* new snapshot */
  }
  return SQLITE_OK;
}

int sqlite3BtreeBeginTrans(Btree *p, int wrflag, int *pSchemaVersion){
  int rc = zsbtBegin(p, wrflag);
  if( rc==SQLITE_OK && wrflag && p->db->nSavepoint>0 ){
    rc = zsbtSavepointTo(p, p->db->nSavepoint);
  }
  if( rc==SQLITE_OK && pSchemaVersion ){
    sqlite3BtreeGetMeta(p, BTREE_SCHEMA_VERSION, (u32*)pSchemaVersion);
  }
  return rc;
}

int sqlite3BtreeCommitPhaseOne(Btree *p, const char *zSuperJrnl){
  (void)p; (void)zSuperJrnl;
  return SQLITE_OK;
}

int sqlite3BtreeCommitPhaseTwo(Btree *p, int bCleanup){
  int zrc = ZS_OK;
  int wasWrite = (p->eTxn==ZS_TRANS_WRITE);
  /* Other statements on this connection may still be reading (a write
  ** statement can commit while a SELECT streams rows); stock btree
  ** downgrades TRANS_WRITE to TRANS_READ then.  Zeroskip has no
  ** in-place downgrade: save cursor positions BEFORE the commit kills
  ** their borrows, then reopen a read snapshot for them to re-seek in. */
  int keepRead = (p->db->nVdbeRead>1) && p->pTxn!=0;

  (void)bCleanup;
  if( keepRead ){
    int rc = zsbtSaveAllCursors(p, 0, 0);
    if( rc!=SQLITE_OK ) return rc;
  }
  if( p->pTxn ){
    if( wasWrite ){
      zrc = zs_txn_commit(&p->pTxn);
    }else{
      zs_txn_abort(&p->pTxn);   /* read snapshot: nothing to commit */
    }
  }
  if( keepRead && zrc==ZS_OK ){
    zsbtUndoReset(p);
    zsbtMaxInvalidate(p);
    p->pMetaTxn = 0; p->metaValid = 0;
    if( zs_db_begin_txn(p->pZs, 1, &p->pTxn)==ZS_OK ){
      p->eTxn = ZS_TRANS_READ;  /* the downgrade */
    }else{
      p->eTxn = ZS_TRANS_NONE;  /* write lock already released above */
      zsbtEndTxn(p);
    }
  }else{
    zsbtEndTxn(p);
  }
  if( zrc!=ZS_OK ){
    /* C-7a (library 2.6.0): a commit that reports an error has an UNKNOWN
    ** outcome -- the terminator may or may not be durable, and the database
    ** is correct either way.  Under the old two-gate protocol a first-gate
    ** failure additionally meant the transaction had NOT happened, and this
    ** function used to inherit that by leaving iDataVersion alone.  It no
    ** longer holds, so the conservative half of the pair is the right one:
    ** bump the data version and drop the meta cache, which makes this
    ** connection re-read rather than trust a cache built before an outcome
    ** it cannot determine.  That is refusing to trust our own state, not
    ** touching the database -- C-7a says report the error and do nothing
    ** else, and in particular never retry the sync and read success as
    ** evidence the data survived. */
    p->iDataVersion++;
    p->pMetaTxn = 0;
    p->metaValid = 0;
    return zsbtErr(zrc);
  }
  if( wasWrite ){
    p->iDataVersion++;
    /* VACUUM asked for a full merge; ordinary repacking is the library's
    ** job now (D-16e at BEGIN), not ours after every commit. */
    if( p->wantCompact ){
      p->wantCompact = 0;
      zs_db_compact(p->pZs);    /* scheduled by VACUUM's CopyFile */
    }
    if( p->syncMs>0 ){
      struct timeval tv;
      sqlite3_int64 ms;
      gettimeofday(&tv, 0);
      ms = (sqlite3_int64)tv.tv_sec*1000 + tv.tv_usec/1000;
      if( p->lastSyncMs==0 ){
        p->lastSyncMs = ms;
      }else if( ms - p->lastSyncMs >= p->syncMs ){
        zs_db_sync(p->pZs);            /* advisory: bounds the loss window */
        p->lastSyncMs = ms;
      }
    }
  }
  return SQLITE_OK;
}

int sqlite3BtreeCommit(Btree *p){
  int rc = sqlite3BtreeCommitPhaseOne(p, 0);
  if( rc==SQLITE_OK ) rc = sqlite3BtreeCommitPhaseTwo(p, 0);
  return rc;
}

int sqlite3BtreeRollback(Btree *p, int tripCode, int writeOnly){
  /* Other statements on this connection may still be reading, exactly as
  ** for a commit (an interrupted or aborted statement rolls back while a
  ** SELECT streams rows); stock's btreeEndTransaction keeps the Btree in
  ** TRANS_READ when db->nVdbeRead>1.  Tripped cursors stay tripped. */
  int keepRead = p->db!=0 && p->db->nVdbeRead>1 && p->pTxn!=0;
  int wasWrite = (p->eTxn==ZS_TRANS_WRITE);

  if( tripCode!=SQLITE_OK ){
    /* active statements must observe the rollback (SQLITE_ABORT_
    ** ROLLBACK); zsbtEndTxn would only leave them silently INVALID */
    zsbtSaveAllCursors(p, tripCode, writeOnly);
  }else if( keepRead ){
    if( zsbtSaveAllCursors(p, 0, 0)!=SQLITE_OK ) keepRead = 0;
  }
  if( p->pTxn ) zs_txn_abort(&p->pTxn);
  if( keepRead ){
    zsbtUndoReset(p);
    zsbtMaxInvalidate(p);
    p->pMetaTxn = 0; p->metaValid = 0;
    if( zs_db_begin_txn(p->pZs, 1, &p->pTxn)==ZS_OK ){
      p->eTxn = ZS_TRANS_READ;
      return SQLITE_OK;
    }
    p->eTxn = ZS_TRANS_NONE;    /* write lock already released above */
  }
  zsbtEndTxn(p);
  return SQLITE_OK;
}

int sqlite3BtreeBeginStmt(Btree *p, int iStatement){
  assert( p->eTxn==ZS_TRANS_WRITE );
  return zsbtSavepointTo(p, iStatement);
}

int sqlite3BtreeSavepoint(Btree *p, int op, int iSavepoint){
  ZsUndo *u;

  if( p==0 || p->eTxn!=ZS_TRANS_WRITE ) return SQLITE_OK;
  assert( op==SAVEPOINT_RELEASE || op==SAVEPOINT_ROLLBACK );
  assert( iSavepoint>=0 || (iSavepoint==-1 && op==SAVEPOINT_ROLLBACK) );
  u = &p->undo;
  if( op==SAVEPOINT_RELEASE ){
    /* Destroy marks iSavepoint and later; entries stay because an
    ** outer savepoint's rollback still replays through them. */
    if( iSavepoint<u->nMark ) u->nMark = iSavepoint;
    return SQLITE_OK;
  }
  if( iSavepoint<0 ){
    /* Transaction-savepoint rollback: undo everything recorded but
    ** keep the transaction open. */
    int rc = zsbtUndoReplay(p, 0);
    u->nMark = 0;
    return rc;
  }
  if( iSavepoint>=u->nMark ){
    /* No writes were recorded after that savepoint opened. */
    return SQLITE_OK;
  }
  /* Replay to the savepoint's watermark; it stays open, later marks
  ** are destroyed. */
  {
    int rc = zsbtUndoReplay(p, u->aMark[iSavepoint]);
    u->nMark = iSavepoint+1;
    return rc;
  }
}
int sqlite3BtreeTxnState(Btree *p){
  return p ? p->eTxn : ZS_TRANS_NONE;
}
int sqlite3BtreeIsInBackup(Btree *p){
  return p->inBackup;
}

/*************************************************************************
** Schema handle (mirrors stock)
*/

void *sqlite3BtreeSchema(Btree *p, int nBytes, void(*xFree)(void *)){
  if( !p->pSchema && nBytes ){
    p->pSchema = sqlite3DbMallocZero(0, nBytes);
    p->xFreeSchema = xFree;
  }
  return p->pSchema;
}
int sqlite3BtreeSchemaLocked(Btree *p){
  (void)p; return SQLITE_OK;
}

#ifndef SQLITE_OMIT_WAL
int sqlite3BtreeCheckpoint(Btree *p, int eMode, int *pnLog, int *pnCkpt){
  (void)p; (void)eMode;
  if( pnLog ) *pnLog = 0;
  if( pnCkpt ) *pnCkpt = 0;
  return SQLITE_OK;
}
#endif

const char *sqlite3BtreeGetFilename(Btree *p){
  return p->zDir ? p->zDir : "";
}
const char *sqlite3BtreeGetJournalname(Btree *p){
  (void)p; return 0;
}

int sqlite3BtreeIncrVacuum(Btree *p){
  (void)p; return SQLITE_DONE;
}

/*************************************************************************
** Tree management
*/

int sqlite3BtreeCreateTable(Btree *p, Pgno *piTable, int flags){
  u8 aKey[8];
  u8 aVal[4];
  int nKey;
  const char *pOld;
  size_t nOld;
  u32 iLast = 1;                /* tree 1 (sqlite_schema) always exists */
  int zrc;
  int rc;

  (void)flags;
  if( p->eTxn!=ZS_TRANS_WRITE ) return SQLITE_INTERNAL;
  nKey = zsbtCounterKey(aKey);
  zrc = zs_txn_fetch(p->pTxn, (const char*)aKey, nKey, 0, 0, &pOld, &nOld, 0);
  if( zrc==ZS_OK && nOld==4 ){
    iLast = zskeyGetTreeId((const u8*)pOld);
  }else if( zrc!=ZS_OK && zrc!=ZS_NOTFOUND ){
    return zsbtErr(zrc);
  }
  iLast++;
  zskeyPutTreeId(aVal, iLast);
  rc = zsbtWrite(p, aKey, nKey, (const char*)aVal, 4, 0);
  if( rc!=SQLITE_OK ) return rc;
  *piTable = (Pgno)iLast;
  return SQLITE_OK;
}
int sqlite3BtreeClearTable(Btree *p, int iTable, i64 *pnChange){
  struct zs_cursor *pZc = 0;
  u8 aPrefix[4];
  const char *k, *v;
  size_t nk, nv;
  i64 n = 0;
  int zrc;
  int rc = SQLITE_OK;

  assert( p->eTxn==ZS_TRANS_WRITE );
  rc = zsbtCheckPinned(p, (u32)iTable, 0);
  if( rc!=SQLITE_OK ) return rc;
  zskeyPutTreeId(aPrefix, (u32)iTable);
  zrc = zs_txn_begin_cursor(p->pTxn, (const char*)aPrefix, 4, &pZc, 0);
  if( zrc!=ZS_OK ) return zsbtErr(zrc);
  {
    ZsKeyBuf keyCopy;
    memset(&keyCopy, 0, sizeof(keyCopy));
    while( (zrc = zs_cursor_next(pZc, &k, &nk, &v, &nv))==ZS_OK ){
      if( nk<4 || memcmp(k, aPrefix, 4)!=0 ) break;
      /* engine-owned copy: the borrow can die mid-txn on rollover */
      keyCopy.n = 0;
      rc = zskeyBufAppend(&keyCopy, (const u8*)k, (int)nk);
      if( rc==SQLITE_OK ) rc = zsbtWrite(p, keyCopy.a, nk, 0, 0, 0);
      if( rc!=SQLITE_OK ) break;
      n++;
    }
    sqlite3_free(keyCopy.a);
  }
  zs_cursor_fini(&pZc);
  if( rc!=SQLITE_OK ) return rc;
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbtErr(zrc);
  if( pnChange ) *pnChange += n;
  return SQLITE_OK;
}

int sqlite3BtreeDropTable(Btree *p, int iTable, int *piMoved){
  *piMoved = 0;                  /* tree ids are never relocated */
  return sqlite3BtreeClearTable(p, iTable, 0);
}

int sqlite3BtreeClearTableOfCursor(BtCursor *pCur){
  return sqlite3BtreeClearTable(pCur->pBtree, (int)pCur->iTree, 0);
}
int sqlite3BtreeTripAllCursors(Btree *p, int errCode, int writeOnly){
  if( p==0 ) return SQLITE_OK;
  return zsbtSaveAllCursors(p, errCode, writeOnly);
}
int sqlite3BtreeNewDb(Btree *p){
  (void)p;                      /* creation is implicit */
  return SQLITE_OK;
}

/*************************************************************************
** Meta values
*/

void sqlite3BtreeGetMeta(Btree *p, int idx, u32 *pValue){
  u8 aKey[8];
  int nKey;
  const char *pVal;
  size_t nVal;
  struct zs_txn *pTmp = 0;
  struct zs_txn *pTxn;
  int zrc;

  *pValue = 0;
  if( idx==BTREE_DATA_VERSION ){
    *pValue = p->iDataVersion;
    return;
  }
  assert( idx>=1 && idx<=15 );
  if( p->pTxn!=0 && p->pTxn==p->pMetaTxn && (p->metaValid & (1<<idx))!=0 ){
    *pValue = p->aMetaVal[idx];
    return;
  }
  if( zsbtEnsureOpen(p)!=SQLITE_OK ) return;
  pTxn = p->pTxn;
  if( pTxn==0 ){
    if( zs_db_begin_txn(p->pZs, 1, &pTmp)!=ZS_OK ) return;
    pTxn = pTmp;
  }
  nKey = zsbtMetaKey(aKey, idx);
  zrc = zs_txn_fetch(pTxn, (const char*)aKey, nKey, 0, 0, &pVal, &nVal, 0);
  if( zrc==ZS_OK && nVal==4 ){
    *pValue = zskeyGetTreeId((const u8*)pVal);   /* 4-byte BE decode */
  }
  /* Only a value read inside the caller's own transaction may be cached:
  ** the pTmp path has no snapshot to scope it to. */
  if( pTmp==0 && p->pTxn!=0 && (zrc==ZS_OK || zrc==ZS_NOTFOUND) ){
    if( p->pTxn!=p->pMetaTxn ){ p->pMetaTxn = p->pTxn; p->metaValid = 0; }
    p->aMetaVal[idx] = *pValue;
    p->metaValid |= (u16)(1<<idx);
  }
  if( pTmp ) zs_txn_abort(&pTmp);
}

int sqlite3BtreeUpdateMeta(Btree *p, int idx, u32 value){
  u8 aKey[8];
  u8 aVal[4];
  int nKey;
  int rc;

  if( p->eTxn!=ZS_TRANS_WRITE ) return SQLITE_INTERNAL;
  assert( idx>=1 && idx<=15 );
  nKey = zsbtMetaKey(aKey, idx);
  zskeyPutTreeId(aVal, value);                   /* 4-byte BE encode */
  rc = zsbtWrite(p, aKey, nKey, (const char*)aVal, 4, 0);
  if( rc==SQLITE_OK ){
    if( p->pTxn!=p->pMetaTxn ){ p->pMetaTxn = p->pTxn; p->metaValid = 0; }
    p->aMetaVal[idx] = value;
    p->metaValid |= (u16)(1<<idx);
  }else{
    p->metaValid = 0;
  }
  return rc;
}

/*************************************************************************
** Cursors
*/

/* A write to a tree that would move a PINNED cursor on that tree
** fails: OP_CursorLock's protection against triggers modifying the
** table an UPDATE is running REPLACE conflict resolution on. */
static int zsbtCheckPinned(Btree *p, u32 iTree, BtCursor *pExcept){
  BtCursor *pC;
  for(pC=p->pCursor; pC; pC=pC->pNext){
    if( pC!=pExcept && pC->isPinned && pC->iTree==iTree ){
      return SQLITE_CONSTRAINT_PINNED;
    }
  }
  return SQLITE_OK;
}

/* Load a borrowed (key,value) as the cursor's current position.
**
** A-4 gives a borrow the lifetime of WHATEVER PRODUCED IT: a fetch
** borrow lives as long as the transaction, a cursor borrow only as long
** as that cursor.  A-4a binds both across a snapshot swap, so a rollover
** mid-transaction does NOT invalidate either -- the outgoing snapshot's
** files must outlive their borrowers.  (An earlier version of this
** comment said borrows die on rollover and called it an open question
** upstream; A-4a answers it normatively.)
**
** The difference between the two lifetimes is load-bearing, because this
** engine finalises a cursor's zs cursor on insert, delete and re-seek --
** so a value borrowed from a cursor STEP can die long before the
** transaction does.  bFromFetch records which kind this is, and
** zsbtUndoHint refuses anything else: an undo entry outlives every
** cursor.  As of library 2.2.0 A-4 states the independence outright -- a
** fetch result MUST survive the end of any cursor on that transaction,
** including one opened after the fetch -- so the hint rests on a promise
** rather than on a reading of one.
**
** The key is copied into aSavedKey regardless, because a deferred use of
** the POSITION (re-seek, delete, insert-overwrite) must not depend on
** either lifetime. */
static int zsbtLoadCurrent(
  BtCursor *pCur,
  const char *k, size_t nk,
  const char *v, size_t nv,
  int bFromFetch
){
  int rc = zsbtSavePosition(pCur, k, nk);
  if( rc!=SQLITE_OK ){
    pCur->eState = ZS_CUR_INVALID;
    return rc;
  }
  pCur->pKey = k;
  pCur->nKey = nk;
  pCur->pVal = v;
  pCur->nVal = nv;
  if( pCur->curIntKey ){
    assert( nk==12 );
    pCur->intKey = zskeyGetRowid((const u8*)k+4);
  }
  pCur->seenEpoch = pCur->pBtree->writeEpoch;
  pCur->eState = ZS_CUR_VALID;
  pCur->skipNextAdv = 0;
  pCur->skipPrevAdv = 0;
  pCur->valSrc = bFromFetch ? ZS_VAL_FETCH : ZS_VAL_CURSOR;
  return SQLITE_OK;
}

/* True if the key belongs to the cursor's tree. */
static int zsbtInTree(BtCursor *pCur, const char *k, size_t nk){
  return nk>=4 && memcmp(k, pCur->aPrefix, 4)==0;
}

/* Compare two keys, memcmp order with length tiebreak. */
static int zsbtKeyCmp(const char *a, size_t na, const char *b, size_t nb){
  size_t n = na<nb ? na : nb;
  int c = memcmp(a, b, n);
  if( c==0 ) c = (na<nb) ? -1 : (na>nb ? 1 : 0);
  return c;
}

/*
** Position pCur at the smallest key >= aKey (strictly > with skipExact)
** within its tree.  aKey MAY be the cursor's own borrowed pKey: the
** replacement zeroskip cursor is opened before the old one is closed,
** so the borrow is valid exactly when it is used (spec rule A-4).
** Returns SQLITE_OK, SQLITE_DONE (no such key; cursor invalidated), or
** an error.
*/
static int zsbtCursorSeekGE(
  BtCursor *pCur,
  const u8 *aKey, size_t nKey,
  int skipExact
){
  Btree *p = pCur->pBtree;
  struct zs_cursor *pNew = 0;
  const char *k, *v;
  size_t nk, nv;
  int zrc;

  assert( p->pTxn!=0 );
  zrc = zs_txn_begin_cursor(p->pTxn, (const char*)aKey, nKey, &pNew,
                            skipExact ? ZS_SKIPROOT : 0);
  if( zrc!=ZS_OK ) return zsbtErr(zrc);
  if( pCur->pZc ) zs_cursor_fini(&pCur->pZc);
  pCur->pZc = pNew;
  pCur->isReverse = 0;
  zrc = zs_cursor_next(pCur->pZc, &k, &nk, &v, &nv);
  if( zrc==ZS_OK && zsbtInTree(pCur, k, nk) ){
    return zsbtLoadCurrent(pCur, k, nk, v, nv, 0);
  }
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbtErr(zrc);
  pCur->eState = ZS_CUR_INVALID;
  return SQLITE_DONE;
}

/*
** Position pCur at the largest in-tree key <= aLimit (strictly < with
** `strict`), or at the very last key of the tree when aLimit is NULL,
** using a zeroskip reverse cursor.  The adopted cursor keeps iterating
** toward smaller keys, so sqlite3BtreePrevious steps it directly.
** Same return contract as zsbtCursorSeekGE.
*/
static int zsbtSeekLE(
  BtCursor *pCur,
  const u8 *aLimit, size_t nLimit,
  int strict
){
  Btree *p = pCur->pBtree;
  struct zs_cursor *pNew = 0;
  const char *k, *v;
  const char *aStart;
  size_t nStart;
  int flags;
  size_t nk, nv;
  int zrc;

  assert( p->pTxn!=0 );
  if( aLimit==0 ){
    /* ZS_CURSOR_PREFIX composes with ZS_REVERSE: the scan begins at the
    ** last key carrying the prefix */
    aStart = (const char*)pCur->aPrefix;
    nStart = 4;
    flags = ZS_REVERSE|ZS_CURSOR_PREFIX;
  }else{
    aStart = (const char*)aLimit;
    nStart = nLimit;
    flags = ZS_REVERSE | (strict ? ZS_SKIPROOT : 0);
  }
  zrc = zs_txn_begin_cursor(p->pTxn, aStart, nStart, &pNew, flags);
  if( zrc!=ZS_OK ) return zsbtErr(zrc);
  if( pCur->pZc ) zs_cursor_fini(&pCur->pZc);
  pCur->pZc = pNew;
  pCur->isReverse = 1;
  zrc = zs_cursor_next(pCur->pZc, &k, &nk, &v, &nv);
  if( zrc==ZS_OK && zsbtInTree(pCur, k, nk) ){
    return zsbtLoadCurrent(pCur, k, nk, v, nv, 0);
  }
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbtErr(zrc);
  pCur->eState = ZS_CUR_INVALID;
  return SQLITE_DONE;
}

/*
** Point flavour of "position at the last in-tree key", for callers
** that usually do not iterate afterwards (rowid allocation on every
** INSERT).  A single ZS_FETCHPREV against a bound above every possible
** suffix: intkey suffixes are exactly 8 bytes (nine 0xFF bytes sort
** above them all), and every index suffix starts with a type byte
** <= 0xFA, plain or DESC-inverted (one 0xFF suffices).  Iteration
** after this re-seeks lazily through the REQUIRESEEK machinery.
*/
static int zsbtPointLast(BtCursor *pCur, int bProbe){
  u8 aMax[13];
  size_t n;
  memcpy(aMax, pCur->aPrefix, 4);
  if( pCur->curIntKey ){
    memset(aMax+4, 0xFF, 9);
    n = 13;
  }else{
    aMax[4] = 0xFF;
    n = 5;
  }
  return zsbtPointFetch(pCur, aMax, n, ZS_FETCHPREV, bProbe);
}

/*
** Point flavour of a GE seek: the smallest in-tree key >= aKey via one
** bare ZS_FETCHNEXT (inclusive as of spec A-12).  Cursor-less load;
** iteration, if any follows, re-seeks lazily.
*/
/*
** Point fetch that positions the cursor.  dirFlag is ZS_FETCHNEXT
** (inclusive GE) or ZS_FETCHPREV (inclusive LE).
**
** With bProbe the fetch asks for ZS_EPHEMERAL, which lets zeroskip
** answer out of the writer's 64KB chunk buffer instead of write()ing it
** to the file first so an mmap pointer can be returned (A-4b).  On a
** bulk load that write-for-visibility is three syscalls per record, so
** the probe is worth roughly 2x.
**
** The pointers die at our next call, so the probe keeps the KEY (copied
** into aSavedKey, which pKey then addresses) and drops the VALUE.  The
** position stays REAL -- Next, Prev, Eof and Delete behave exactly as
** they would after an ordinary fetch, which is what a lazily-positioned
** cursor got wrong -- and only a payload read has to go back to the
** store, through zsbtLoadValue.
*/
static int zsbtPointFetch(
  BtCursor *pCur,
  const u8 *aKey, size_t nKey,
  int dirFlag,
  int bProbe
){
  Btree *p = pCur->pBtree;
  const char *k, *v;
  size_t nk, nv;
  int zrc;

  assert( p->pTxn!=0 );
  zrc = zs_txn_fetch(p->pTxn, (const char*)aKey, nKey, &k, &nk, &v, &nv,
                     dirFlag | (bProbe ? ZS_EPHEMERAL : 0));
  if( zrc==ZS_OK && zsbtInTree(pCur, k, nk) ){
    if( pCur->pZc ) zs_cursor_fini(&pCur->pZc);
    if( bProbe ){
      int rc = zsbtSavePosition(pCur, k, nk);   /* copy before the next call */
      if( rc!=SQLITE_OK ){
        pCur->eState = ZS_CUR_INVALID;
        return rc;
      }
      pCur->pKey = (const char*)pCur->aSavedKey;   /* engine-owned */
      pCur->nKey = (size_t)pCur->nSavedKey;
      pCur->pVal = 0;
      pCur->nVal = 0;
      pCur->valSrc = ZS_VAL_ABSENT;
      if( pCur->curIntKey ){
        assert( nk==12 );
        pCur->intKey = zskeyGetRowid((const u8*)pCur->aSavedKey+4);
      }
      pCur->seenEpoch = p->writeEpoch;
      pCur->eState = ZS_CUR_VALID;
      pCur->skipNextAdv = 0;
      pCur->skipPrevAdv = 0;
      return SQLITE_OK;
    }
    zsbtLoadCurrent(pCur, k, nk, v, nv, 1);
    return SQLITE_OK;
  }
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbtErr(zrc);
  pCur->eState = ZS_CUR_INVALID;
  return SQLITE_DONE;
}

/* Fetch the value for a position taken by an ephemeral probe.  Durable
** this time: what it returns is read across VDBE calls. */
static int zsbtLoadValue(BtCursor *pCur){
  Btree *p = pCur->pBtree;
  ZsKeyBuf sought;
  const char *k, *v;
  size_t nk, nv;
  int zrc, rc;

  assert( pCur->valSrc==ZS_VAL_ABSENT );
  assert( pCur->eState==ZS_CUR_VALID );
  memset(&sought, 0, sizeof(sought));
  rc = zskeyBufAppend(&sought, pCur->aSavedKey, pCur->nSavedKey);
  if( rc!=SQLITE_OK ) return rc;
  zrc = zs_txn_fetch(p->pTxn, (const char*)sought.a, (size_t)sought.n,
                     &k, &nk, &v, &nv, 0);
  sqlite3_free(sought.a);
  if( zrc!=ZS_OK ) return zrc==ZS_NOTFOUND ? SQLITE_CORRUPT_BKPT : zsbtErr(zrc);
  pCur->pVal = v;
  pCur->nVal = nv;
  pCur->valSrc = ZS_VAL_FETCH;   /* zs_txn_fetch: txn lifetime (A-4) */
  return SQLITE_OK;
}

/* True when a probe is worth taking: inside the write transaction, where
** the chunk buffer holds unflushed bytes. */
static int zsbtCanProbe(BtCursor *pCur){
  return pCur->pBtree->eTxn==ZS_TRANS_WRITE;
}

static int zsbtPointGE(BtCursor *pCur, const u8 *aKey, size_t nKey){
  return zsbtPointFetch(pCur, aKey, nKey, ZS_FETCHNEXT, 0);
}

/*
** Point flavour of zsbtSeekLE for moveto fallbacks: position at the
** largest in-tree key <= aKey via a single ZS_FETCHPREV fetch instead
** of opening a merge cursor (whose open cost is proportional to file
** arms plus the pending write-set).  The position is loaded without a
** zeroskip cursor; iteration, if any follows, re-seeks lazily.
*/
static int zsbtPointLE(BtCursor *pCur, const u8 *aKey, size_t nKey){
  return zsbtPointFetch(pCur, aKey, nKey, ZS_FETCHPREV, 0);
}

/* Remember aKey as the cursor's saved position (heap copy). */
static int zsbtSavePosition(BtCursor *pCur, const char *aKey, size_t nKey){
  if( (size_t)pCur->nSavedKey<nKey || pCur->aSavedKey==0 ){
    u8 *aNew = sqlite3Realloc(pCur->aSavedKey, nKey ? nKey : 1);
    if( aNew==0 ) return SQLITE_NOMEM_BKPT;
    pCur->aSavedKey = aNew;
  }
  memcpy(pCur->aSavedKey, aKey, nKey);
  pCur->nSavedKey = (int)nKey;
  return SQLITE_OK;
}

/*
** Re-establish the cursor's position after the underlying data may
** have changed (write epoch bump) or the position was saved
** (REQUIRESEEK).  On SQLITE_OK, *pExact says whether the cursor landed
** exactly on the remembered key; landing elsewhere means the cursor
** now sits on the remembered key's successor.
*/
static int zsbtCursorReseek(BtCursor *pCur, int *pExact){
  int rc;

  *pExact = 0;
  assert( pCur->eState==ZS_CUR_VALID || pCur->eState==ZS_CUR_REQUIRESEEK );
  /* aSavedKey always mirrors the position (zsbtLoadCurrent), but the
  ** seek reloads it, so remember what we sought */
  {
    ZsKeyBuf sought;
    memset(&sought, 0, sizeof(sought));
    rc = zskeyBufAppend(&sought, pCur->aSavedKey, pCur->nSavedKey);
    if( rc!=SQLITE_OK ) return rc;
    rc = zsbtCursorSeekGE(pCur, sought.a, (size_t)sought.n, 0);
    if( rc==SQLITE_OK ){
      *pExact = (pCur->nKey==(size_t)sought.n)
             && memcmp(pCur->pKey, sought.a, pCur->nKey)==0;
    }
    sqlite3_free(sought.a);
  }
  return rc;
}

int sqlite3BtreeCursor(
  Btree *p,
  Pgno iTable,
  int wrFlag,
  struct KeyInfo *pKeyInfo,
  BtCursor *pCur
){
  assert( p->pTxn!=0 );
  pCur->pBtree = p;
  pCur->iTree = (u32)iTable;
  zskeyPutTreeId(pCur->aPrefix, (u32)iTable);
  pCur->pKeyInfo = pKeyInfo;
  pCur->curIntKey = (pKeyInfo==0);
  pCur->wrFlag = (u8)wrFlag;
  pCur->eState = ZS_CUR_INVALID;
  pCur->pNext = p->pCursor;
  p->pCursor = pCur;
  return SQLITE_OK;
}

BtCursor *sqlite3BtreeFakeValidCursor(void){
  /* A whole zeroed struct, not stock's one-byte trick: our
  ** CursorHasMoved also reads pBtree (NULL here = "never moves"). */
  static BtCursor fakeCursor = {ZS_CUR_VALID};
  assert( offsetof(BtCursor, eState)==0 );
  return &fakeCursor;
}

int sqlite3BtreeCursorSize(void){
  return ROUND8(sizeof(BtCursor));
}

#ifdef SQLITE_DEBUG
int sqlite3BtreeClosesWithCursor(Btree *pBtree, BtCursor *pCur){
  BtCursor *p;
  for(p=pBtree->pCursor; p; p=p->pNext){
    if( p==pCur ) return 1;
  }
  return 0;
}
#endif

void sqlite3BtreeCursorZero(BtCursor *p){
  memset(p, 0, sizeof(*p));
}

void sqlite3BtreeCursorHintFlags(BtCursor *pCur, unsigned x){
  assert( x==BTREE_SEEK_EQ || x==BTREE_BULKLOAD || x==0 );
  pCur->hints = x;
}

#ifdef SQLITE_ENABLE_CURSOR_HINTS
void sqlite3BtreeCursorHint(BtCursor *pCur, int eHintType, ...){
  (void)pCur; (void)eHintType;
}
#ifdef SQLITE_DEBUG
BtCursor *sqlite3BtreeCursorHintTblCsr(BtCursor *pCur){
  (void)pCur; return 0;
}
#endif
#endif

int sqlite3BtreeCloseCursor(BtCursor *pCur){
  Btree *p = pCur->pBtree;
  if( p ){
    BtCursor **pp;
    for(pp=&p->pCursor; *pp; pp=&(*pp)->pNext){
      if( *pp==pCur ){ *pp = pCur->pNext; break; }
    }
    zsbtCursorFini(pCur);
    sqlite3_free(pCur->aSavedKey);
    pCur->aSavedKey = 0;
    sqlite3_free(pCur->encBuf.a);
    memset(&pCur->encBuf, 0, sizeof(pCur->encBuf));
    if( pCur->pUnpacked ){
      sqlite3DbFree(p->db, pCur->pUnpacked);
      pCur->pUnpacked = 0;
    }
    pCur->pBtree = 0;
  }
  pCur->eState = ZS_CUR_INVALID;
  return SQLITE_OK;
}

/* The tree's append bound, seeded on demand, or 0 if there is none.
**
** INVARIANT: a valid bound names a key that IS IN THE TREE, and no key in
** the tree is above it.  So it can be used as a position, not merely as a
** proof of absence -- which is the whole point, because positioning from
** it costs no fetch at all.
**
** It was stale-high once (deletes lowered the true maximum and left this
** alone, which is safe for proving absence and useless for positioning).
** Exactness is maintained in ONE place: zsbtWrite, the funnel every
** mutation goes through, drops the bound when a delete removes a key at
** or above it.  That covers sqlite3BtreeDelete, ClearTable and DropTable
** (both delete key by key through the funnel), and the old-key delete an
** index replace does.  Two paths do not go through the funnel and both
** already invalidate wholesale: zsbtUndoReplay (a savepoint rollback can
** restore a deleted maximum) and every transaction boundary.
**
** Write transactions only: a read transaction has no bound to raise and
** zsbtMaxInvalidate drops the cache at every transaction boundary.  A
** backup writes into its destination outside the funnel, which is safe
** because inBackup refuses a write transaction, so there is no bound.
*/
static ZsTreeMax *zsbtMaxEnsure(BtCursor *pCur){
  Btree *p = pCur->pBtree;
  ZsTreeMax *pM;

  if( p->eTxn!=ZS_TRANS_WRITE ) return 0;
  pM = zsbtMaxSlot(p, pCur->iTree, 1);
  if( pM==0 || pM->valid ) return pM;
  {
    /* the largest in-tree key of the current snapshot; the sentinel is
    ** zsbtPointLast's, for the reasons given there */
    u8 aBound[13];
    const char *k, *v;
    size_t nk, nv, nBound;
    int zrc;
    memcpy(aBound, pCur->aPrefix, 4);
    if( pCur->curIntKey ){
      memset(aBound+4, 0xFF, 9);
      nBound = 13;
    }else{
      aBound[4] = 0xFF;
      nBound = 5;
    }
    zrc = zs_txn_fetch(p->pTxn, (const char*)aBound, nBound,
                       &k, &nk, &v, &nv, ZS_FETCHPREV);
    if( zrc==ZS_OK && zsbtInTree(pCur, k, nk) ){
      pM->max.n = 0;
      if( zskeyBufAppend(&pM->max, (const u8*)k, (int)nk)==SQLITE_OK ){
        pM->valid = 1;
      }
    }else if( zrc==ZS_NOTFOUND || zrc==ZS_DONE
           || (zrc==ZS_OK && !zsbtInTree(pCur, k, nk)) ){
      pM->max.n = 0;             /* empty tree: everything is above it */
      pM->valid = 1;
    }
  }
  return pM;
}

/* The bound, when aKey is provably above every key in its tree; else 0.
** An empty tree answers with a valid bound of length 0, which every key
** is above. */
static ZsTreeMax *zsbtAboveTreeMax(BtCursor *pCur, const u8 *aKey,
                                   size_t nKey){
  ZsTreeMax *pM = zsbtMaxEnsure(pCur);
  if( pM!=0 && pM->valid
   && zsbtKeyCmp((const char*)aKey, nKey,
                 (const char*)pM->max.a, (size_t)pM->max.n)>0 ){
    return pM;
  }
  return 0;
}

/* Establish a position on a key already known to be in the tree, without
** fetching it: the bound is such a key.  The value is left unread -- the
** ZS_VAL_ABSENT path fetches it durably if anything asks -- so an
** append-shaped probe, which never looks at the row it lands beside,
** costs no I/O whatever. */
static int zsbtPositionAt(BtCursor *pCur, const u8 *aKey, size_t nKey){
  Btree *p = pCur->pBtree;
  int rc;

  if( pCur->pZc ) zs_cursor_fini(&pCur->pZc);
  rc = zsbtSavePosition(pCur, (const char*)aKey, nKey);
  if( rc!=SQLITE_OK ){
    pCur->eState = ZS_CUR_INVALID;
    return rc;
  }
  pCur->pKey = (const char*)pCur->aSavedKey;
  pCur->nKey = (size_t)pCur->nSavedKey;
  pCur->pVal = 0;
  pCur->nVal = 0;
  pCur->valSrc = ZS_VAL_ABSENT;
  if( pCur->curIntKey ){
    assert( pCur->nSavedKey==12 );
    pCur->intKey = zskeyGetRowid((const u8*)pCur->aSavedKey+4);
  }
  pCur->seenEpoch = p->writeEpoch;
  pCur->eState = ZS_CUR_VALID;
  pCur->skipNextAdv = 0;
  pCur->skipPrevAdv = 0;
  return SQLITE_OK;
}

int sqlite3BtreeTableMoveto(BtCursor *pCur, i64 intKey, int bias, int *pRes){
  u8 aKey[12];
  int rc;
  int bProbe;

  (void)bias;
  assert( pCur->curIntKey );
  memcpy(aKey, pCur->aPrefix, 4);
  zskeyPutRowid(aKey+4, intKey);
  bProbe = zsbtCanProbe(pCur);
  /* A key above the tree's append bound cannot exist, and the bound IS
  ** the largest key in the tree -- which is exactly the entry a miss
  ** above it has to land on.  So both fetches go: no forward one to
  ** learn the key is absent, and no backward one to find the row below
  ** it.  An append-shaped bulk load pays no lookup at all here, where it
  ** used to pay two over the whole file set. */
  {
    ZsTreeMax *pM = zsbtAboveTreeMax(pCur, aKey, 12);
    if( pM ){
      if( pM->max.n>0 ){
        rc = zsbtPositionAt(pCur, pM->max.a, (size_t)pM->max.n);
        if( rc!=SQLITE_OK ) return rc;
      }else{
        pCur->eState = ZS_CUR_INVALID;      /* empty tree */
      }
      *pRes = -1;                /* on a smaller entry, or an empty tree */
      return SQLITE_OK;
    }
  }
  rc = zsbtPointFetch(pCur, aKey, 12, ZS_FETCHNEXT, bProbe);
  if( rc==SQLITE_DONE ){
    rc = zsbtPointFetch(pCur, aKey, 12, ZS_FETCHPREV, bProbe);
    if( rc==SQLITE_DONE ){         /* empty tree */
      *pRes = -1;
      return SQLITE_OK;
    }
    if( rc!=SQLITE_OK ) return rc;
    *pRes = -1;                    /* on an entry smaller than intKey */
    return SQLITE_OK;
  }
  if( rc!=SQLITE_OK ) return rc;
  *pRes = (pCur->intKey==intKey) ? 0 : 1;
  return SQLITE_OK;
}

int sqlite3BtreeIndexMoveto(BtCursor *pCur, UnpackedRecord *pUnKey, int *pRes){
  u8 *aKey;
  size_t nKey;
  int rc;

  assert( !pCur->curIntKey );
  pCur->encBuf.n = 0;
  rc = zskeyBufAppend(&pCur->encBuf, pCur->aPrefix, 4);
  if( rc==SQLITE_OK ){
    rc = zskeyEncodeUnpackedBuf(pUnKey, pUnKey->nField, &pCur->encBuf);
  }
  if( rc!=SQLITE_OK ){
    if( rc==SQLITE_ERROR ){
      sqlite3_log(SQLITE_ERROR,
        "zeroskip: unsupported collation or text encoding in index key");
    }
    return rc;
  }
  aKey = pCur->encBuf.a;
  nKey = (size_t)pCur->encBuf.n;
  pUnKey->eqSeen = 0;
  /* Append-shaped conflict checks: Found/NotFound/NoConflict
  ** (default_rc==0) only consume res!=0, and a key above the tree's
  ** largest live key cannot exist.  See zsbtMaxEnsure for the bound's
  ** invariant.  A miss falls through to the real seek.
  **
  ** This answers without positioning, which is all these callers need --
  ** and it is also why the prefix-equal probe in sqlite3BtreeInsert does
  ** not fire on a plain INSERT: OP_IdxInsert carries
  ** OPFLAG_USESEEKRESULT, so it passes the res=-1 set here and takes the
  ** seekResult!=0 path. */
  if( pUnKey->default_rc==0 && zsbtAboveTreeMax(pCur, aKey, nKey)!=0 ){
    /* Found/NotFound/NoConflict consume only res!=0, never the landing
    ** position, so this one still answers without positioning. */
    pCur->eState = ZS_CUR_INVALID;
    *pRes = -1;
    return SQLITE_OK;
  }
  if( pUnKey->default_rc<0 && (pCur->hints & BTREE_SEEK_EQ)!=0 ){
    /* Entries whose fields all equal the sought prefix count as LESS
    ** than the target (SeekGT/SeekLE).  Every real continuation of the
    ** encoding starts with a type byte <= 0xFA, so appending 0xFF
    ** yields a key past every prefix-equal entry.  BTREE_SEEK_EQ
    ** cursors need to know whether an equal entry exists at all
    ** (UnpackedRecord.eqSeen), which the landing position cannot tell
    ** us here -- probe for it first. */
    const char *k, *v;
    size_t nk, nv;
    int zrc = zs_txn_fetch(pCur->pBtree->pTxn, (const char*)aKey, nKey,
                           &k, &nk, &v, &nv, ZS_FETCHNEXT);
    if( zrc==ZS_OK && nk>=nKey && memcmp(k, aKey, nKey)==0 ){
      pUnKey->eqSeen = 1;          /* exact entry or an extension */
    }
    if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ){
      return zsbtErr(zrc);
    }
  }
  if( pUnKey->default_rc<0 ){
    rc = zskeyBufAppend(&pCur->encBuf, (const u8*)"\xff", 1);
    if( rc!=SQLITE_OK ) return rc;
    aKey = pCur->encBuf.a;
    nKey = (size_t)pCur->encBuf.n;
  }
  rc = zsbtPointGE(pCur, aKey, nKey);
  if( rc==SQLITE_DONE ){
    if( pUnKey->default_rc==0 ){
      /* Found/NotFound/NoConflict consume only res!=0, never the
      ** landing position: skip the LE fallback's point lookup. */
      *pRes = -1;
      return SQLITE_OK;
    }
    /* nothing at or after the sought key: land on the largest smaller
    ** entry, or report an empty tree */
    rc = zsbtPointLE(pCur, aKey, nKey);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
    *pRes = -1;
    return SQLITE_OK;
  }
  if( rc!=SQLITE_OK ){
    return rc;
  }
  if( pUnKey->default_rc<0 ){
    *pRes = 1;                   /* found key is > target by construction */
  }else if( pCur->nKey>=nKey && memcmp(pCur->pKey, aKey, nKey)==0 ){
    /* Field encodings are prefix-free, so a byte-prefix match means
    ** every sought field compared equal -- exactly when RecordCompare
    ** would return default_rc. */
    *pRes = pUnKey->default_rc;
    pUnKey->eqSeen = 1;
  }else{
    *pRes = 1;
  }
  return SQLITE_OK;
}

int sqlite3BtreeCursorHasMoved(BtCursor *pCur){
  if( pCur->pBtree==0 ) return 0;      /* the fake always-valid cursor */
  return pCur->eState!=ZS_CUR_VALID
      || pCur->seenEpoch!=pCur->pBtree->writeEpoch;
}

int sqlite3BtreeCursorRestore(BtCursor *pCur, int *pDifferentRow){
  int rc, exact;

  assert( pCur->eState!=ZS_CUR_FAULT );
  if( pCur->eState==ZS_CUR_VALID
   && pCur->seenEpoch==pCur->pBtree->writeEpoch ){
    *pDifferentRow = 0;
    return SQLITE_OK;
  }
  if( pCur->eState==ZS_CUR_INVALID ){
    *pDifferentRow = 1;
    return SQLITE_OK;
  }
  rc = zsbtCursorReseek(pCur, &exact);
  if( rc==SQLITE_DONE ){
    /* The row is gone and has no successor in this tree.  Stock leaves
    ** the cursor on the nearest cell, so a reverse scan can still step
    ** back; land on the predecessor and let Previous deliver it. */
    ZsKeyBuf gone;
    memset(&gone, 0, sizeof(gone));
    rc = zskeyBufAppend(&gone, pCur->aSavedKey, pCur->nSavedKey);
    if( rc!=SQLITE_OK ) return rc;
    rc = zsbtSeekLE(pCur, (const u8*)gone.a, (size_t)gone.n, 1);
    sqlite3_free(gone.a);
    if( rc==SQLITE_DONE ) rc = SQLITE_OK;      /* tree empty: INVALID */
    else if( rc==SQLITE_OK ) pCur->skipPrevAdv = 1;
    *pDifferentRow = 1;
    return rc;
  }
  if( rc!=SQLITE_OK ) return rc;
  if( !exact ){
    /* landed on the successor of a vanished row: a subsequent Next
    ** must deliver this row, not step past it (stock's skipNext) */
    pCur->skipNextAdv = 1;
  }
  *pDifferentRow = !exact;
  return SQLITE_OK;
}

int sqlite3BtreeDelete(BtCursor *pCur, u8 flags){
  Btree *p = pCur->pBtree;
  int rc, exact;

  (void)flags;
  assert( pCur->wrFlag & BTREE_WRCSR );
  rc = zsbtCheckPinned(p, pCur->iTree, pCur);
  if( rc!=SQLITE_OK ) return rc;
  if( pCur->eState!=ZS_CUR_VALID
   || pCur->seenEpoch!=p->writeEpoch ){
    rc = zsbtCursorReseek(pCur, &exact);
    if( rc!=SQLITE_OK || !exact ) return rc==SQLITE_OK ? SQLITE_CORRUPT_BKPT
                                                       : rc;
  }
  /* aSavedKey mirrors the current position (engine-owned): borrows
  ** can die mid-txn if the active file rolls over */
  rc = zsbtWrite(p, pCur->aSavedKey, (size_t)pCur->nSavedKey, 0, 0, pCur);
  if( rc!=SQLITE_OK ) return rc;
  /* Logical position: the gap where the row was.  A subsequent Next
  ** re-seeks from the saved key and lands on the successor. */
  pCur->eState = ZS_CUR_REQUIRESEEK;
  return SQLITE_OK;
}

int sqlite3BtreeInsert(
  BtCursor *pCur,
  const BtreePayload *pX,
  int flags,
  int seekResult
){
  Btree *p = pCur->pBtree;
  int rc;

  (void)seekResult;
  assert( pCur->wrFlag & BTREE_WRCSR );

  /* The OP_RowCell/OP_Insert pair: our sqlite3BtreeTransferRow already
  ** performed the whole insertion, so the PREFORMAT completion is a
  ** no-op (the cursor is already on the transferred row). */
  if( flags & BTREE_PREFORMAT ) return SQLITE_OK;

  rc = zsbtCheckPinned(p, pCur->iTree, pCur);
  if( rc!=SQLITE_OK ) return rc;

  /* The cursor's logical position after insert is the inserted key.
  ** Establishing it eagerly would cost a full lookup per insert; the
  ** REQUIRESEEK machinery re-seeks lazily if a read actually needs it,
  ** which bulk-load flows never do. */
  if( pCur->curIntKey ){
    u8 aKey[12];
    const char *pData = (const char*)pX->pData;
    char *aTmp = 0;
    size_t nData = (size_t)pX->nData;

    memcpy(aKey, pCur->aPrefix, 4);
    zskeyPutRowid(aKey+4, pX->nKey);
    if( pX->nZero>0 ){
      aTmp = sqlite3_malloc64(nData + pX->nZero);
      if( aTmp==0 ) return SQLITE_NOMEM_BKPT;
      memcpy(aTmp, pData, nData);
      memset(aTmp+nData, 0, pX->nZero);
      pData = aTmp;
      nData += pX->nZero;
    }
    rc = zsbtWrite(p, aKey, 12, pData, nData, pCur);
    sqlite3_free(aTmp);
    if( rc!=SQLITE_OK ) return rc;
    rc = zsbtSavePosition(pCur, (const char*)aKey, 12);
    if( rc!=SQLITE_OK ) return rc;
  }else{
    /* Index tree: key = encoded record, value = the original record so
    ** that reads never need a decoder.  The key is built in the
    ** cursor's reusable scratch buffer. */
    pCur->encBuf.n = 0;
    rc = zskeyBufAppend(&pCur->encBuf, pCur->aPrefix, 4);
    if( rc==SQLITE_OK ){
      rc = zskeyEncodeRecordBuf(pCur->pKeyInfo, (int)pX->nKey, pX->pKey,
                                &pCur->pUnpacked, &pCur->encBuf);
    }
    if( rc!=SQLITE_OK ){
      if( rc==SQLITE_ERROR ){
        /* The statement aborts with a generic "SQL logic error"; the
        ** specific cause is only reachable via the error log. */
        sqlite3_log(SQLITE_ERROR,
          "zeroskip: unsupported collation or text encoding in index key");
      }
      return rc;
    }
    /* Stock overwrite semantics: when loc==0, an existing entry whose
    ** first pX->nMem fields equal the new row's (or the entry under
    ** the cursor, for BTREE_SAVEPOSITION) is REPLACED, not appended
    ** beside -- UPDATE on WITHOUT ROWID tables emits no IdxDelete and
    ** relies on this. */
    if( seekResult==0 && (flags & BTREE_SAVEPOSITION)!=0 ){
      /* the cursor is on the row being replaced */
      if( pCur->eState==ZS_CUR_VALID || pCur->eState==ZS_CUR_REQUIRESEEK ){
        int exact = (pCur->eState==ZS_CUR_VALID
                     && pCur->seenEpoch==p->writeEpoch);
        if( !exact ) rc = zsbtCursorReseek(pCur, &exact);
        else rc = SQLITE_OK;
        if( rc==SQLITE_OK && exact
         && ( pCur->nSavedKey!=pCur->encBuf.n
           || memcmp(pCur->aSavedKey, pCur->encBuf.a, pCur->nSavedKey)!=0 ) ){
          rc = zsbtWrite(p, pCur->aSavedKey, (size_t)pCur->nSavedKey,
                         0, 0, pCur);
          if( rc!=SQLITE_OK ) return rc;
        }
      }
    }else if( seekResult==0 && pX->nMem>0 ){
      /* probe for an entry prefix-equal on the first nMem fields */
      UnpackedRecord r;
      ZsKeyBuf probe;
      memset(&probe, 0, sizeof(probe));
      r.pKeyInfo = pCur->pKeyInfo;
      r.aMem = pX->aMem;
      r.nField = pX->nMem;
      r.default_rc = 0;
      r.eqSeen = 0;
      rc = zskeyBufAppend(&probe, pCur->aPrefix, 4);
      if( rc==SQLITE_OK ) rc = zskeyEncodeUnpackedBuf(&r, pX->nMem, &probe);
      if( rc==SQLITE_OK ){
        const char *k, *v;
        size_t nk, nv;
        int zrc2 = zs_txn_fetch(p->pTxn, (const char*)probe.a,
                                (size_t)probe.n, &k, &nk, &v, &nv,
                                ZS_FETCHNEXT);
        if( zrc2==ZS_OK && nk>=(size_t)probe.n
         && memcmp(k, probe.a, probe.n)==0
         && ( nk!=(size_t)pCur->encBuf.n
           || memcmp(k, pCur->encBuf.a, nk)!=0 ) ){
          probe.n = 0;                 /* reuse as an owned key copy */
          rc = zskeyBufAppend(&probe, (const u8*)k, (int)nk);
          if( rc==SQLITE_OK ){
            rc = zsbtWrite(p, probe.a, (size_t)probe.n, 0, 0, 0);
          }
        }else if( zrc2!=ZS_OK && zrc2!=ZS_DONE && zrc2!=ZS_NOTFOUND ){
          rc = zsbtErr(zrc2);
        }
      }
      sqlite3_free(probe.a);
      if( rc!=SQLITE_OK ) return rc;
    }
    rc = zsbtWrite(p, pCur->encBuf.a, (size_t)pCur->encBuf.n,
                   (const char*)pX->pKey, (size_t)pX->nKey, pCur);
    if( rc!=SQLITE_OK ) return rc;
    rc = zsbtSavePosition(pCur, (const char*)pCur->encBuf.a,
                          (size_t)pCur->encBuf.n);
    if( rc!=SQLITE_OK ) return rc;
  }
  if( pCur->pZc ) zs_cursor_fini(&pCur->pZc);
  pCur->eState = ZS_CUR_REQUIRESEEK;
  return SQLITE_OK;
}

int sqlite3BtreeFirst(BtCursor *pCur, int *pRes){
  int rc = zsbtCursorSeekGE(pCur, pCur->aPrefix, 4, 0);
  if( rc==SQLITE_DONE ){
    *pRes = 1;
    return SQLITE_OK;
  }
  if( rc!=SQLITE_OK ) return rc;
  *pRes = 0;
  return SQLITE_OK;
}

int sqlite3BtreeIsEmpty(BtCursor *pCur, int *pRes){
  struct zs_cursor *pZc = 0;
  const char *k, *v;
  size_t nk, nv;
  int zrc;

  zrc = zs_txn_begin_cursor(pCur->pBtree->pTxn,
                            (const char*)pCur->aPrefix, 4, &pZc, 0);
  if( zrc!=ZS_OK ) return zsbtErr(zrc);
  zrc = zs_cursor_next(pZc, &k, &nk, &v, &nv);
  *pRes = !(zrc==ZS_OK && zsbtInTree(pCur, k, nk));
  zs_cursor_fini(&pZc);
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbtErr(zrc);
  return SQLITE_OK;
}

int sqlite3BtreeLast(BtCursor *pCur, int *pRes){
  int rc = zsbtPointLast(pCur, zsbtCanProbe(pCur));
  if( rc==SQLITE_DONE ){
    *pRes = 1;
    return SQLITE_OK;
  }
  if( rc!=SQLITE_OK ) return rc;
  *pRes = 0;
  return SQLITE_OK;
}

int sqlite3BtreeNext(BtCursor *pCur, int flags){
  const char *k, *v;
  size_t nk, nv;
  int zrc;

  int skipAdv;

  (void)flags;
  if( pCur->eState==ZS_CUR_FAULT ) return pCur->skipNext;
  if( pCur->eState==ZS_CUR_INVALID ) return SQLITE_DONE;
  skipAdv = pCur->skipNextAdv;
  pCur->skipNextAdv = 0;
  pCur->skipPrevAdv = 0;      /* only Previous consumes it */
  if( pCur->eState==ZS_CUR_REQUIRESEEK
   || pCur->seenEpoch!=pCur->pBtree->writeEpoch
   || pCur->isReverse
   || pCur->pZc==0 ){                 /* position loaded without a cursor
                                      ** (post-Insert txn fetch) */
    int rc, exact;
    rc = zsbtCursorReseek(pCur, &exact);
    if( rc==SQLITE_DONE ) return SQLITE_DONE;
    if( rc!=SQLITE_OK ) return rc;
    if( !exact ){
      /* the remembered row is gone; its successor IS the next row */
      return SQLITE_OK;
    }
    if( skipAdv ) return SQLITE_OK;   /* restore already advanced us */
  }else if( skipAdv ){
    return SQLITE_OK;
  }
  zrc = zs_cursor_next(pCur->pZc, &k, &nk, &v, &nv);
  if( zrc==ZS_OK && zsbtInTree(pCur, k, nk) ){
    return zsbtLoadCurrent(pCur, k, nk, v, nv, 0);
  }
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbtErr(zrc);
  pCur->eState = ZS_CUR_INVALID;
  return SQLITE_DONE;
}

int sqlite3BtreeEof(BtCursor *pCur){
  return pCur->eState!=ZS_CUR_VALID;
}

int sqlite3BtreePrevious(BtCursor *pCur, int flags){
  const char *k, *v;
  size_t nk, nv;
  int zrc;
  int rc;

  (void)flags;
  if( pCur->eState==ZS_CUR_FAULT ) return pCur->skipNext;
  if( pCur->eState==ZS_CUR_INVALID ) return SQLITE_DONE;
  pCur->skipNextAdv = 0;      /* the strict-LE reseek is direction-correct */
  if( pCur->skipPrevAdv ){
    /* restore already landed us on the predecessor */
    pCur->skipPrevAdv = 0;
    return SQLITE_OK;
  }
  if( pCur->eState==ZS_CUR_VALID
   && pCur->seenEpoch==pCur->pBtree->writeEpoch
   && pCur->isReverse
   && pCur->pZc!=0 ){
    /* already running backwards: one reverse step */
    zrc = zs_cursor_next(pCur->pZc, &k, &nk, &v, &nv);
    if( zrc==ZS_OK && zsbtInTree(pCur, k, nk) ){
      return zsbtLoadCurrent(pCur, k, nk, v, nv, 0);
    }
    if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbtErr(zrc);
    pCur->eState = ZS_CUR_INVALID;
    return SQLITE_DONE;
  }
  /* direction change, stale epoch, or saved position: strictly-less
  ** seek from the remembered key (aSavedKey always mirrors it), which
  ** the seek itself reloads -- copy what we seek from */
  {
    ZsKeyBuf from;
    memset(&from, 0, sizeof(from));
    rc = zskeyBufAppend(&from, pCur->aSavedKey, pCur->nSavedKey);
    if( rc!=SQLITE_OK ) return rc;
    rc = zsbtSeekLE(pCur, from.a, (size_t)from.n, 1);
    sqlite3_free(from.a);
  }
  return rc;
}

i64 sqlite3BtreeIntegerKey(BtCursor *pCur){
  assert( pCur->curIntKey );
  return pCur->intKey;
}
void sqlite3BtreeCursorPin(BtCursor *pCur){
  assert( !pCur->isPinned );
  pCur->isPinned = 1;
}
void sqlite3BtreeCursorUnpin(BtCursor *pCur){
  assert( pCur->isPinned );
  pCur->isPinned = 0;
}
i64 sqlite3BtreeOffset(BtCursor *pCur){
  (void)pCur; return 0;
}

int sqlite3BtreePayload(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
  assert( pCur->eState==ZS_CUR_VALID );
  if( pCur->valSrc==ZS_VAL_ABSENT ){
    int rc = zsbtLoadValue(pCur);
    if( rc!=SQLITE_OK ) return rc;
  }
  if( (u64)offset+amt > pCur->nVal ) return SQLITE_CORRUPT_BKPT;
  memcpy(pBuf, pCur->pVal+offset, amt);
  return SQLITE_OK;
}
const void *sqlite3BtreePayloadFetch(BtCursor *pCur, u32 *pAmt){
  assert( pCur->eState==ZS_CUR_VALID );
  /* No error path here (stock returns the in-page pointer), so a failed
  ** reload reports an empty payload and the caller's own checks fail the
  ** statement rather than reading a stale pointer. */
  if( pCur->valSrc==ZS_VAL_ABSENT && zsbtLoadValue(pCur)!=SQLITE_OK ){
    *pAmt = 0;
    return "";
  }
  *pAmt = (u32)pCur->nVal;
  return pCur->pVal;
}
u32 sqlite3BtreePayloadSize(BtCursor *pCur){
  assert( pCur->eState==ZS_CUR_VALID );
  if( pCur->valSrc==ZS_VAL_ABSENT && zsbtLoadValue(pCur)!=SQLITE_OK ) return 0;
  return (u32)pCur->nVal;
}
sqlite3_int64 sqlite3BtreeMaxRecordSize(BtCursor *pCur){
  return pCur->pBtree->db->aLimit[SQLITE_LIMIT_LENGTH];
}

int sqlite3BtreeIntegrityCheck(
  sqlite3 *db,
  Btree *p,
  Pgno *aRoot,
  sqlite3_value *aCnt,
  int nRoot,
  int mxErr,
  int *pnErr,
  char **pzOut
){
  struct zs_cursor *pZc = 0;
  sqlite3_str *pStr;
  i64 *aCount;
  const char *k, *v;
  size_t nk, nv;
  u32 iLastStray = 0;
  int bPartial;
  int nErr = 0;
  int i;
  int zrc;

  assert( nRoot>0 && aCnt!=0 );
  *pzOut = 0;
  *pnErr = 0;
  bPartial = (aRoot[0]==0);     /* strays are expected then */

  aCount = sqlite3MallocZero(sizeof(i64)*nRoot);
  if( aCount==0 ) return SQLITE_NOMEM_BKPT;
  pStr = sqlite3_str_new(db);

  if( zsbtEnsureOpen(p)!=SQLITE_OK || p->pTxn==0 ){
    sqlite3_str_appendf(pStr, "cannot open zeroskip database\n");
    nErr++;
    goto ic_done;
  }
  if( zs_db_check_consistency(p->pZs)!=ZS_OK ){
    sqlite3_str_appendf(pStr, "zeroskip consistency check failed\n");
    nErr++;
  }

  zrc = zs_txn_begin_cursor(p->pTxn, 0, 0, &pZc, 0);
  if( zrc!=ZS_OK ){
    sqlite3_str_appendf(pStr, "zeroskip scan failed (%s)\n",
                        zs_strerror(zrc));
    nErr++;
    goto ic_done;
  }
  while( nErr<mxErr && (zrc = zs_cursor_next(pZc, &k, &nk, &v, &nv))==ZS_OK ){
    u32 iTree;
    if( nk<4 ){
      sqlite3_str_appendf(pStr, "key shorter than a tree id (%d bytes)\n",
                          (int)nk);
      nErr++;
      continue;
    }
    iTree = zskeyGetTreeId((const u8*)k);
    if( iTree==0 ) continue;             /* meta space */
    for(i=0; i<nRoot; i++){
      if( aRoot[i]==(Pgno)iTree ){
        aCount[i]++;
        break;
      }
    }
    if( i==nRoot && !bPartial && iTree!=iLastStray ){
      sqlite3_str_appendf(pStr, "rows in unexpected tree %u\n", iTree);
      nErr++;
      iLastStray = iTree;
    }
  }
  zs_cursor_fini(&pZc);
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ){
    sqlite3_str_appendf(pStr, "zeroskip scan failed (%s)\n",
                        zs_strerror(zrc));
    nErr++;
  }

ic_done:
  for(i=0; i<nRoot; i++){
    sqlite3MemSetArrayInt64((Mem*)aCnt, i, aCount[i]);
  }
  sqlite3_free(aCount);
  if( nErr>0 ){
    /* trim the trailing newline for pragma formatting */
    if( sqlite3_str_length(pStr)>0 ){
      pStr->nChar--;
    }
    *pzOut = sqlite3_str_finish(pStr);
    if( *pzOut==0 ) return SQLITE_NOMEM_BKPT;
  }else{
    sqlite3_free(sqlite3_str_finish(pStr));
  }
  *pnErr = nErr;
  return SQLITE_OK;
}

struct Pager *sqlite3BtreePager(Btree *p){
  return p->pFakePager;
}

i64 sqlite3BtreeRowCountEst(BtCursor *pCur){
  (void)pCur; return 1000000;
}

#ifndef SQLITE_OMIT_INCRBLOB
int sqlite3BtreePayloadChecked(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
  (void)pCur; (void)offset; (void)amt; (void)pBuf;
  return SQLITE_ERROR;
}
int sqlite3BtreePutData(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
  (void)pCur; (void)offset; (void)amt; (void)pBuf;
  return SQLITE_ERROR;
}
void sqlite3BtreeIncrblobCursor(BtCursor *pCur){
  (void)pCur;
}
#endif

void sqlite3BtreeClearCursor(BtCursor *pCur){
  pCur->eState = ZS_CUR_INVALID;
}

int sqlite3BtreeSetVersion(Btree *pBtree, int iVersion){
  (void)pBtree; (void)iVersion;
  return SQLITE_OK;
}

int sqlite3BtreeCursorHasHint(BtCursor *pCur, unsigned int mask){
  return (pCur->hints & mask)!=0;
}

int sqlite3BtreeIsReadonly(Btree *p){
  return p->isReadonly;
}

int sqlite3HeaderSizeBtree(void){
  return 0;
}

#ifdef SQLITE_DEBUG
sqlite3_uint64 sqlite3BtreeSeekCount(Btree *pBt){
  (void)pBt; return 0;
}
#endif

#ifndef NDEBUG
int sqlite3BtreeCursorIsValid(BtCursor *pCur){
  return pCur && pCur->eState==ZS_CUR_VALID;
}
#endif
int sqlite3BtreeCursorIsValidNN(BtCursor *pCur){
  assert( pCur!=0 );
  return pCur->eState==ZS_CUR_VALID;
}

int sqlite3BtreeCount(sqlite3 *db, BtCursor *pCur, i64 *pnEntry){
  struct zs_cursor *pZc = 0;
  const char *k, *v;
  size_t nk, nv;
  i64 n = 0;
  int zrc;

  zrc = zs_txn_begin_cursor(pCur->pBtree->pTxn,
                            (const char*)pCur->aPrefix, 4, &pZc, 0);
  if( zrc!=ZS_OK ) return zsbtErr(zrc);
  while( (zrc = zs_cursor_next(pZc, &k, &nk, &v, &nv))==ZS_OK ){
    if( !zsbtInTree(pCur, k, nk) ) break;
    n++;
    if( db && AtomicLoad(&db->u1.isInterrupted) ){
      zs_cursor_fini(&pZc);
      return SQLITE_ABORT;
    }
  }
  zs_cursor_fini(&pZc);
  if( zrc!=ZS_OK && zrc!=ZS_DONE && zrc!=ZS_NOTFOUND ) return zsbtErr(zrc);
  *pnEntry = n;
  return SQLITE_OK;
}

#ifdef SQLITE_TEST
int sqlite3BtreeCursorInfo(BtCursor *pCur, int *aResult, int upCnt){
  (void)pCur; (void)aResult; (void)upCnt;
  return SQLITE_OK;
}
void sqlite3BtreeCursorList(Btree *p){
  (void)p;
}
#endif

int sqlite3BtreeTransferRow(BtCursor *pDest, BtCursor *pSrc, i64 iKey){
  BtreePayload x;
  u32 nData;
  const void *pData;

  assert( pSrc->eState==ZS_CUR_VALID );
  pData = sqlite3BtreePayloadFetch(pSrc, &nData);
  memset(&x, 0, sizeof(x));
  if( pDest->curIntKey ){
    x.nKey = iKey;
    x.pData = pData;
    x.nData = (int)nData;
  }else{
    x.pKey = pData;
    x.nKey = (i64)nData;
  }
  return sqlite3BtreeInsert(pDest, &x, 0, 0);
}

void sqlite3BtreeClearCache(Btree *p){
  (void)p;
}

/*************************************************************************
** Interface for backup_zs.c (see btree_zs.h)
*/

struct zs_db *sqlite3ZsBtreeDb(Btree *p){
  if( zsbtEnsureOpen(p)!=SQLITE_OK ) return 0;
  return p->pZs;
}

/* Run the repack cascade from idle time, for a handle opened with
** zs_norepack.  Safe to call on any handle: with the cascade armed
** should_repack is normally false, so this is a no-op rather than an error.
** Must not be called inside a transaction -- a merge takes the write lock.
**
** Bounded because the intended caller is a post-response delayed-work slot,
** not a benchmark: one merge rewrites a whole generation and cannot be
** interrupted, so "catch up completely" is the wrong contract for anything
** with a latency budget.  See btree_zs.h for why *pbBehind is part of the
** interface rather than left to the caller to re-derive. */
int sqlite3ZsRepackCatchUp(sqlite3 *db, const char *zDb, int nMaxMerges,
                           int *pnMerges, int *pbBehind){
  struct zs_db *pZs;
  int iDb, n = 0;

  if( pnMerges ) *pnMerges = 0;
  if( pbBehind ) *pbBehind = 0;
  iDb = zDb ? sqlite3FindDbName(db, zDb) : 0;
  if( iDb<0 || db->aDb[iDb].pBt==0 ) return SQLITE_ERROR;
  pZs = sqlite3ZsBtreeDb(db->aDb[iDb].pBt);
  if( pZs==0 ) return SQLITE_ERROR;
  /* An unbounded call still gets a backstop, because a cascade that will not
  ** converge is a bug in either layer and an infinite loop is a worse way to
  ** report it than a wrong count. */
  if( nMaxMerges<=0 ) nMaxMerges = 1000;
  while( n<nMaxMerges && zs_db_should_repack(pZs) ){
    int zrc = zs_db_repack(pZs);
    if( zrc!=ZS_OK ) return zsbtErr(zrc);
    n++;
  }
  if( pnMerges ) *pnMerges = n;
  if( pbBehind ) *pbBehind = zs_db_should_repack(pZs) ? 1 : 0;
  return SQLITE_OK;
}

/* Convert the active generation from idle time.  See btree_zs.h: this is the
** conversion half of the latency story, which ZS_NOAUTOREPACK does not cover.
** Must not be called inside a transaction -- a conversion takes the write
** lock. */
int sqlite3ZsSeal(sqlite3 *db, const char *zDb){
  struct zs_db *pZs;
  int iDb, zrc;

  iDb = zDb ? sqlite3FindDbName(db, zDb) : 0;
  if( iDb<0 || db->aDb[iDb].pBt==0 ) return SQLITE_ERROR;
  pZs = sqlite3ZsBtreeDb(db->aDb[iDb].pBt);
  if( pZs==0 ) return SQLITE_ERROR;
  zrc = zs_db_seal(pZs);
  return zrc==ZS_OK ? SQLITE_OK : zsbtErr(zrc);
}

int sqlite3ZsStats(sqlite3 *db, const char *zDb,
                   sqlite3_uint64 *aOut, int nOut){
  struct zs_db_stats st;
  struct zs_db *pZs;
  int iDb;

  if( aOut==0 || nOut<8 ) return SQLITE_MISUSE;
  memset(aOut, 0, nOut*sizeof(*aOut));
  iDb = zDb ? sqlite3FindDbName(db, zDb) : 0;
  if( iDb<0 || db->aDb[iDb].pBt==0 ) return SQLITE_ERROR;
  pZs = sqlite3ZsBtreeDb(db->aDb[iDb].pBt);
  if( pZs==0 ) return SQLITE_ERROR;
  if( zs_db_stats(pZs, &st)!=ZS_OK ) return SQLITE_ERROR;
  aOut[0] = st.repacks;
  aOut[1] = st.repack_records;
  aOut[2] = st.repack_bytes;
  aOut[3] = st.repack_ns;
  aOut[4] = st.conversions;
  aOut[5] = st.convert_records;
  aOut[6] = st.convert_bytes;
  aOut[7] = st.convert_ns;
  return SQLITE_OK;
}

struct zs_txn *sqlite3ZsBtreeTxn(Btree *p){
  return p->pTxn;
}

void sqlite3ZsBtreeInvalidate(Btree *p){
  p->writeEpoch++;
  p->iDataVersion++;
  zsbtMaxInvalidate(p);          /* content changed behind the funnel */
}

void sqlite3ZsBtreeSetInBackup(Btree *p, int flag){
  p->inBackup = (u8)(flag!=0);
}


/* Schedule a full zs_db_compact after the current write transaction
** commits (compaction cannot run under an open write txn). */
void sqlite3ZsBtreeScheduleCompact(Btree *p){
  p->wantCompact = 1;
}

#endif /* SQLITE_ZEROSKIP */
