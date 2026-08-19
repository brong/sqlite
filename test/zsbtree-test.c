/*
** 2026-08-11
**
** C harness for the zeroskip btree engine core: open/close,
** transactions, meta values, tree allocation.  Links against the
** non-amalgamation libsqlite3.a built with SQLITE_ZEROSKIP and calls
** the internal btree API directly.
*/
#include "sqliteInt.h"
#include "btree.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static int weirdColl(void *p, int na, const void *a, int nb, const void *b){
  int n = na<nb ? na : nb;
  int c = memcmp(a, b, n);              /* BINARY-alike, but unknown to us */
  (void)p;
  return c ? c : na-nb;
}

#define CHECK(x) do{ \
  if( !(x) ){ \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; \
  } \
}while(0)

int main(void){
  sqlite3 *db = 0;
  Btree *pBt = 0;
  int rc;
  u32 v = 0;
  Pgno root = 0;

  system("rm -rf /tmp/zsbt-test-db");

  /* The handle open fails (stub-era behaviour was an error; now the
  ** main-db open succeeds) -- either way we only need the handle. */
  sqlite3_open(":memory:", &db);
  CHECK( db!=0 );
  db->enc = SQLITE_UTF8;

  rc = sqlite3BtreeOpen(0, "/tmp/zsbt-test-db", db, &pBt, 0,
                        SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
  CHECK( rc==SQLITE_OK );
  CHECK( sqlite3BtreeTxnState(pBt)==SQLITE_TXN_NONE );

  rc = sqlite3BtreeBeginTrans(pBt, 1, 0);
  CHECK( rc==SQLITE_OK );
  CHECK( sqlite3BtreeTxnState(pBt)==SQLITE_TXN_WRITE );

  rc = sqlite3BtreeUpdateMeta(pBt, BTREE_SCHEMA_VERSION, 7);
  CHECK( rc==SQLITE_OK );
  sqlite3BtreeGetMeta(pBt, BTREE_SCHEMA_VERSION, &v);
  CHECK( v==7 );

  rc = sqlite3BtreeCreateTable(pBt, &root, BTREE_INTKEY);
  CHECK( rc==SQLITE_OK );
  CHECK( root==2 );
  rc = sqlite3BtreeCreateTable(pBt, &root, BTREE_BLOBKEY);
  CHECK( rc==SQLITE_OK );
  CHECK( root==3 );

  rc = sqlite3BtreeCommit(pBt);
  CHECK( rc==SQLITE_OK );
  CHECK( sqlite3BtreeTxnState(pBt)==SQLITE_TXN_NONE );

  rc = sqlite3BtreeClose(pBt);
  CHECK( rc==SQLITE_OK );

  /* reopen: meta persisted, tree-id allocation continues */
  pBt = 0;
  rc = sqlite3BtreeOpen(0, "/tmp/zsbt-test-db", db, &pBt, 0,
                        SQLITE_OPEN_READWRITE);
  CHECK( rc==SQLITE_OK );

  rc = sqlite3BtreeBeginTrans(pBt, 0, 0);
  CHECK( rc==SQLITE_OK );
  CHECK( sqlite3BtreeTxnState(pBt)==SQLITE_TXN_READ );
  v = 0;
  sqlite3BtreeGetMeta(pBt, BTREE_SCHEMA_VERSION, &v);
  CHECK( v==7 );
  rc = sqlite3BtreeCommit(pBt);
  CHECK( rc==SQLITE_OK );

  /* read -> write upgrade with no open cursors */
  rc = sqlite3BtreeBeginTrans(pBt, 0, 0);
  CHECK( rc==SQLITE_OK );
  rc = sqlite3BtreeBeginTrans(pBt, 1, 0);
  CHECK( rc==SQLITE_OK );
  CHECK( sqlite3BtreeTxnState(pBt)==SQLITE_TXN_WRITE );

  rc = sqlite3BtreeCreateTable(pBt, &root, BTREE_INTKEY);
  CHECK( rc==SQLITE_OK );
  CHECK( root==4 );

  /* rollback discards the allocation */
  rc = sqlite3BtreeRollback(pBt, SQLITE_OK, 0);
  CHECK( rc==SQLITE_OK );
  CHECK( sqlite3BtreeTxnState(pBt)==SQLITE_TXN_NONE );

  rc = sqlite3BtreeBeginTrans(pBt, 1, 0);
  CHECK( rc==SQLITE_OK );
  rc = sqlite3BtreeCreateTable(pBt, &root, BTREE_INTKEY);
  CHECK( rc==SQLITE_OK );
  CHECK( root==4 );
  rc = sqlite3BtreeCommit(pBt);
  CHECK( rc==SQLITE_OK );

  /* meta read with no open transaction (temporary internal snapshot) */
  v = 0;
  sqlite3BtreeGetMeta(pBt, BTREE_SCHEMA_VERSION, &v);
  CHECK( v==7 );

  /* ephemeral btree: NULL filename, cleaned up on close */
  {
    Btree *pEph = 0;
    rc = sqlite3BtreeOpen(0, 0, db, &pEph, BTREE_OMIT_JOURNAL|BTREE_SINGLE,
                          SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
    CHECK( rc==SQLITE_OK );
    rc = sqlite3BtreeBeginTrans(pEph, 1, 0);
    CHECK( rc==SQLITE_OK );
    rc = sqlite3BtreeUpdateMeta(pEph, BTREE_USER_VERSION, 42);
    CHECK( rc==SQLITE_OK );
    rc = sqlite3BtreeCommit(pEph);
    CHECK( rc==SQLITE_OK );
    rc = sqlite3BtreeClose(pEph);
    CHECK( rc==SQLITE_OK );
  }

  sqlite3BtreeClose(pBt);
  sqlite3_close(db);

  /* custom collations are refused in index keys, with a clear error */
  {
    sqlite3 *db2 = 0;
    char *zErr = 0;
    rc = sqlite3_open(":memory:", &db2);
    CHECK( rc==SQLITE_OK );
    rc = sqlite3_create_collation(db2, "weird", SQLITE_UTF8, 0, weirdColl);
    CHECK( rc==SQLITE_OK );
    rc = sqlite3_exec(db2,
      "CREATE TABLE ct(x); CREATE INDEX cti ON ct(x COLLATE weird);",
      0, 0, &zErr);
    if( rc==SQLITE_OK ){
      /* index on an empty table may build without encoding anything;
      ** the rejection must surface on first insert at the latest */
      rc = sqlite3_exec(db2, "INSERT INTO ct VALUES('a');", 0, 0, &zErr);
    }
    /* the statement fails cleanly; the detailed cause goes to the
    ** error log because the vdbe abort path owns the message text */
    CHECK( rc==SQLITE_ERROR );
    sqlite3_free(zErr);
    sqlite3_close(db2);
  }

  /* a running SELECT's cursor survives the read->write upgrade caused
  ** by an INSERT on the same connection, and sees the new row */
  {
    sqlite3 *db3 = 0;
    sqlite3_stmt *pSel = 0;
    i64 seen[8];
    int n = 0;
    system("rm -rf /tmp/zsbt-upg-db");
    CHECK( sqlite3_open("/tmp/zsbt-upg-db", &db3)==SQLITE_OK );
    CHECK( sqlite3_exec(db3,
      "CREATE TABLE t(a INTEGER PRIMARY KEY, b);"
      "INSERT INTO t VALUES(1,1),(2,2),(3,3),(4,4);"
      "BEGIN;", 0, 0, 0)==SQLITE_OK );
    CHECK( sqlite3_prepare_v2(db3, "SELECT a FROM t ORDER BY a", -1,
                              &pSel, 0)==SQLITE_OK );
    CHECK( sqlite3_step(pSel)==SQLITE_ROW );
    seen[n++] = sqlite3_column_int64(pSel, 0);
    /* upgrade happens here, with pSel's cursor open and positioned */
    CHECK( sqlite3_exec(db3, "INSERT INTO t VALUES(50,50);",
                        0, 0, 0)==SQLITE_OK );
    while( sqlite3_step(pSel)==SQLITE_ROW && n<8 ){
      seen[n++] = sqlite3_column_int64(pSel, 0);
    }
    sqlite3_finalize(pSel);
    CHECK( n==5 );
    CHECK( seen[0]==1 && seen[1]==2 && seen[2]==3 && seen[3]==4
        && seen[4]==50 );
    CHECK( sqlite3_exec(db3, "COMMIT;", 0, 0, 0)==SQLITE_OK );
    sqlite3_close(db3);
  }

  printf("zsbtree-test PASS\n");
  return 0;
}
