/*
** 2026-08-12
**
** Key-value benchmark for the zeroskip storage engine, shaped to
** mirror ../zeroskip2's zsbench workloads so the SQL layer's overhead
** over raw zeroskip is directly measurable: same "key%08d" keys, same
** value payloads, same transaction batching, same pseudorandom fetch
** order, best-of-reps timing.
**
** Usage: zskvbench [--dir PATH] [-n N] [--value N] [--reps N] [--rowid]
**
** The default table is WITHOUT ROWID with a TEXT primary key.  --rowid
** uses an INTEGER PRIMARY KEY table instead, which is the commoner
** SQLite shape and the only one that exercises rowid allocation -- and
** so the only one where the engine's ZS_EPHEMERAL probe can show up.
** Measuring only the default hides that difference entirely.
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>

/* Sanitizers cost 3-4x and say nothing about themselves at runtime. */
#if defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define ZSKV_INSTRUMENTED "  [ASAN -- NOT A PERFORMANCE RESULT]"
# endif
#endif
#if !defined(ZSKV_INSTRUMENTED) && defined(__SANITIZE_ADDRESS__)
# define ZSKV_INSTRUMENTED "  [ASAN -- NOT A PERFORMANCE RESULT]"
#endif
#ifndef ZSKV_INSTRUMENTED
# define ZSKV_INSTRUMENTED ""
#endif

static int nrecs = 20000;
static int reps = 3;
static size_t valsize = 100;
static char workdir[1024] = "/tmp";
static const char *zInit = 0;      /* SQL run after every open (--init) */
static const char *zUri = 0;       /* URI query string appended to the path */
static int doVacuum = 0;           /* VACUUM between load and read benches */
static const char *zOnly = 0;      /* --only N: just the N-per-txn store row */
static int deferRepack = 0;        /* --defer-repack: cascade off the write path,
                                   ** then time the catch-up separately */
static int nOpens = 0;             /* --opens N: time N open+first-read cycles */
static int opensPer = 1;           /* --opens-per N: records per transaction in
                                   ** the --opens fixture.  It is the governing
                                   ** variable: the replay an open pays is
                                   ** bounded by SPANS, so how the writer
                                   ** committed decides what a pointer table is
                                   ** worth -- a cliff, not a curve. */
static const char *zBuildUri = 0;  /* --build-uri: URI for --opens' BUILD phase,
                                   ** so a database can be imported under one
                                   ** setting and read under another -- which is
                                   ** the real shape: an import, then the flag
                                   ** turned on for a read-mostly life */

static double now(void){
  struct timeval tv;
  gettimeofday(&tv, 0);
  return tv.tv_sec + tv.tv_usec/1e6;
}

static void cleanup(const char *zDir){
  char cmd[1200];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", zDir);
  system(cmd);
}

static int bRowid = 0;         /* --rowid: INTEGER PRIMARY KEY table */
static int bRandom = 0;        /* --random: store the keys out of order */

/* --random stores the SAME key set as the ascending fixture, in a scrambled
** order.  Not decoration:
**
** An ascending key order is the easy case for an append-only store's in-memory
** index -- a commit's run appends past the end of the delta and the D-13b fold
** costs nothing -- and it is the only case our rowid fixture has ever measured.
** Real index trees are the other case: an index key is an encoded column value,
** so its arrival order has nothing to do with its sort order, and every
** secondary index in a real schema is fed that way.  Library 2.8.0 found a
** quadratic in the private index that ascending keys largely hid (2.04s of
** merging against 6.45s at a 64MB rollover), which is the argument for having
** this row at all: the shape we had was the shape that could not see it.
**
** The permutation is x -> (x * odd) mod 2^k, k the smallest power of two at or
** above nrecs, skipping results at or above nrecs.  Multiplying by an odd
** constant modulo a power of two is a bijection, so every key in [0,nrecs)
** comes out exactly once and the loop is bounded by 2^k iterations.  Keeping
** the key SET identical is the point -- same records, same bytes, same file
** sizes, so an ascending/random pair differs only in arrival order. */
static unsigned permMask = 0;
static unsigned permCur = 0;
static void perm_reset(void){
  unsigned m = 1;
  while( m < (unsigned)nrecs ) m <<= 1;
  permMask = m - 1;
  permCur = 0;
}
static int perm_next(void){
  for(;;){
    unsigned v = (permCur * 2654435769u) & permMask;
    permCur++;
    if( v < (unsigned)nrecs ) return (int)v;
  }
}

static sqlite3 *open_fresh(const char *zDir){
  sqlite3 *db;
  int rc;
  mkdir(workdir, 0755);          /* tolerate a missing parent */
  cleanup(zDir);
  if( zUri || deferRepack ){
    char zFile[1400];
    /* --defer-repack is zs_norepack=1 plus the catch-up call, so it composes
    ** with whatever --uri already asked for (zs_nosync, typically). */
    snprintf(zFile, sizeof(zFile), "file:%s?%s%s%s", zDir,
             zUri ? zUri : "",
             (zUri && deferRepack) ? "&" : "",
             deferRepack ? "zs_norepack=1" : "");
    rc = sqlite3_open_v2(zFile, &db,
           SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_URI, 0);
  }else{
    rc = sqlite3_open(zDir, &db);
  }
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "open %s failed\n", zDir);
    exit(1);
  }
  if( zInit && sqlite3_exec(db, zInit, 0, 0, 0)!=SQLITE_OK ){
    fprintf(stderr, "init: %s\n", sqlite3_errmsg(db));
    exit(1);
  }
  if( sqlite3_exec(db, bRowid
        ? "CREATE TABLE kv(k INTEGER PRIMARY KEY, v BLOB);"
        : "CREATE TABLE kv(k TEXT PRIMARY KEY, v BLOB) WITHOUT ROWID;",
        0, 0, 0)!=SQLITE_OK ){
    fprintf(stderr, "create: %s\n", sqlite3_errmsg(db));
    exit(1);
  }
  return db;
}

static void insert_n(sqlite3 *db, char *val, int per){
  sqlite3_stmt *pIns;
  int done = 0;
  if( sqlite3_prepare_v2(db, "INSERT INTO kv VALUES(?1,?2)", -1,
                         &pIns, 0)!=SQLITE_OK ){
    fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
    exit(1);
  }
  perm_reset();
  while( done<nrecs ){
    int i;
    if( per>1 ) sqlite3_exec(db, "BEGIN", 0, 0, 0);
    for(i=0; i<per && done<nrecs; i++, done++){
      char k[32];
      int key = bRandom ? perm_next() : done;
      if( bRowid ){
        sqlite3_bind_int64(pIns, 1, (sqlite3_int64)key + 1);
      }else{
        snprintf(k, sizeof(k), "key%08d", key);
        sqlite3_bind_text(pIns, 1, k, -1, SQLITE_TRANSIENT);
      }
      sqlite3_bind_blob(pIns, 2, val, (int)valsize, SQLITE_STATIC);
      if( sqlite3_step(pIns)!=SQLITE_DONE ){
        fprintf(stderr, "insert: %s\n", sqlite3_errmsg(db));
        exit(1);
      }
      sqlite3_reset(pIns);
    }
    if( per>1 ) sqlite3_exec(db, "COMMIT", 0, 0, 0);
  }
  sqlite3_finalize(pIns);
}

/* What the library rewrote underneath this run (A-17, library 2.2.0).
**
** A store appends each record once; a conversion then rewrites its whole
** generation in key order and a repack rewrites what it merges, so the bytes
** a bulk load really writes are a multiple of the bytes it stored.  That
** multiple is invisible from SQL and is most of what separates this engine's
** bulk-store row from the btree's, so the benchmark reports it rather than
** leaving it to be guessed at.  Zeroskip builds only; the stock engine has
** nothing to report. */
#ifdef SQLITE_ZEROSKIP
extern int sqlite3ZsStats(sqlite3*, const char*, sqlite3_uint64*, int);
extern int sqlite3ZsRepackCatchUp(sqlite3*, const char*, int*);
static int grab_rewrites(sqlite3 *db, sqlite3_uint64 *a){
  return sqlite3ZsStats(db, 0, a, 8)==SQLITE_OK;
}
/* The other half of --defer-repack: with the cascade off the write path the
** load leaves a pile of files, and the question is what finishing the job
** afterwards costs against having paid it per-commit.  Timed apart because
** that is the whole point -- the totals are what decide it, and on a
** filesystem where a page-cache write is nearly free they come out level
** while on one where bytes and unlinks cost real time they do not. */
static double catch_up(sqlite3 *db, int *pnMerges){
  double t0 = now();
  double dt;
  sqlite3_stmt *pChk;
  const unsigned char *z;
  if( sqlite3ZsRepackCatchUp(db, 0, pnMerges)!=SQLITE_OK ){
    fprintf(stderr, "catch-up failed\n");
    exit(1);
  }
  dt = now() - t0;
  /* Untimed, and deliberately not optional: this is the only thing in the
  ** tier that drives a merge from OUR side rather than the library's, so
  ** every run of it checks that what came back is still a database. */
  if( sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &pChk, 0)!=SQLITE_OK
   || sqlite3_step(pChk)!=SQLITE_ROW
   || (z = sqlite3_column_text(pChk, 0))==0
   || strcmp((const char*)z, "ok")!=0 ){
    fprintf(stderr, "integrity_check after catch-up: %s\n",
            sqlite3_errmsg(db));
    exit(1);
  }
  sqlite3_finalize(pChk);
  return dt;
}
#else
static int grab_rewrites(sqlite3 *db, sqlite3_uint64 *a){
  (void)db; (void)a; return 0;
}
static double catch_up(sqlite3 *db, int *pnMerges){
  (void)db; if( pnMerges ) *pnMerges = 0; return 0.0;
}
#endif

/* `stored` is KEY+VALUE bytes, which is not what the library counts as a
** file's bytes -- it frames each record -- so the multiple below is a
** ratio of comparable-to-itself figures across runs rather than a number
** to hold against upstream's.  The MB and ms are exact. */
static void print_rewrites(const sqlite3_uint64 *a, double stored){
  double rw = (double)a[2] + (double)a[6];
  printf("  %-34s%.1fx of stored  (%llu conversions %.0fMB %.0fms;"
         " %llu repacks %.0fMB %.0fms; stored %.0fMB)\n",
         "rewritten", stored>0 ? rw/stored : 0.0,
         (unsigned long long)a[4], (double)a[6]/1e6, (double)a[7]/1e6,
         (unsigned long long)a[0], (double)a[2]/1e6, (double)a[3]/1e6,
         stored/1e6);
}

static void bench_store(int per){
  char dir[1200];
  char label[64];
  char *val = malloc(valsize);
  double best = 1e18;
  sqlite3_uint64 aStats[8];
  int haveStats = 0;
  double bestCatch = 1e18;
  int nMerges = 0;
  int r;

  memset(val, 'v', valsize);
  if( per==1 ){
    snprintf(label, sizeof(label), "store, one txn each");
  }else{
    snprintf(label, sizeof(label), "store, %d per txn", per);
  }
  printf("  %-34s", label);
  fflush(stdout);
  snprintf(dir, sizeof(dir), "%s/sqlbatch", workdir);
  for(r=0; r<reps; r++){
    sqlite3 *db = open_fresh(dir);
    double t0 = now();
    insert_n(db, val, per);
    double dt = now() - t0;
    if( dt<best ) best = dt;
    /* On the last rep only, and while the handle is still open: the
    ** counters are per handle and each rep opens a fresh database, so
    ** every rep starts from zero. */
    if( deferRepack ){
      int nm = 0;
      double dtc = catch_up(db, &nm);
      if( dtc<bestCatch ){ bestCatch = dtc; nMerges = nm; }
    }
    if( r==reps-1 ) haveStats = grab_rewrites(db, aStats);
    sqlite3_close(db);
    cleanup(dir);
  }
  printf("%8.0f/s  %5.2fs\n", nrecs/best, best);
  if( deferRepack ){
    printf("  %-34s%5.2fs load + %5.2fs catch-up (%d merges) = %5.2fs total\n",
           "deferred cascade", best, bestCatch, nMerges, best + bestCatch);
  }
  /* Only when something was actually rewritten: at 20k records in one big
  ** transaction that is nothing, and a row of zeroes per store row would
  ** bury the cases that matter.  Small transactions are exactly where it
  ** matters, because each commit can start a generation -- a 32% drop in
  ** the 1-per-txn row on ZFS went unexplained until this line was printed
  ** for that row instead of only for the 1000-per-txn one. */
  if( haveStats && (aStats[2] || aStats[6]) ){
    print_rewrites(aStats, (double)nrecs * (double)(11 + valsize));
  }
  free(val);
}

static void bench_fetch_and_scan(void){
  char dir[1200];
  char *val = malloc(valsize);
  sqlite3 *db;
  sqlite3_stmt *pSel;
  double t0, dt;
  int i, hits = 0;

  memset(val, 'v', valsize);
  snprintf(dir, sizeof(dir), "%s/sqlfetch", workdir);
  db = open_fresh(dir);
  insert_n(db, val, 1000);

  if( doVacuum ){
    double t0 = now();
    if( sqlite3_exec(db, "VACUUM", 0, 0, 0)!=SQLITE_OK ){
      fprintf(stderr, "vacuum: %s\n", sqlite3_errmsg(db));
      exit(1);
    }
    printf("  %-34s%8s  %5.2fs\n", "vacuum", "-", now()-t0);
  }

  printf("  %-34s", "fetch");
  fflush(stdout);
  sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k=?1", -1, &pSel, 0);
  t0 = now();
  for(i=0; i<nrecs; i++){
    char k[32];
    if( bRowid ){
      sqlite3_bind_int64(pSel, 1, (sqlite3_int64)((i*7919)%nrecs) + 1);
    }else{
      snprintf(k, sizeof(k), "key%08d", (i*7919)%nrecs);
      sqlite3_bind_text(pSel, 1, k, -1, SQLITE_TRANSIENT);
    }
    if( sqlite3_step(pSel)==SQLITE_ROW ) hits++;
    sqlite3_reset(pSel);
  }
  dt = now() - t0;
  printf("%8.0f/s  %5.2fs  %d hits\n", nrecs/dt, dt, hits);
  sqlite3_finalize(pSel);

  printf("  %-34s", "scan");
  fflush(stdout);
  sqlite3_prepare_v2(db, "SELECT count(*), sum(length(v)) FROM kv", -1,
                     &pSel, 0);
  t0 = now();
  if( sqlite3_step(pSel)!=SQLITE_ROW ){
    fprintf(stderr, "scan: %s\n", sqlite3_errmsg(db));
    exit(1);
  }
  dt = now() - t0;
  printf("%8.0f/s  %5.2fs  %d rows\n",
         sqlite3_column_int(pSel, 0)/dt, dt, sqlite3_column_int(pSel, 0));
  sqlite3_finalize(pSel);

  sqlite3_close(db);
  cleanup(dir);
  free(val);
}

/* Open latency, which is the metric a Cyrus-shaped deployment lives on and
** which nothing else here measures: many short-lived processes, each opening
** the database, doing a little work and exiting.  A snapshot open replays the
** active file's spans, so its cost grows with them -- upstream prices an idle
** 1000-span unordered file at 0.111ms against 0.046ms sealed, and D-9d caps
** the span count at 1023, which bounds the pointer table's open-side prize at
** about 2.4x.  Whether that beats the table's cost during writes depends
** entirely on the open:write ratio, so this measures our side of it.
**
** The build phase commits ONE record per transaction on purpose: that is what
** accumulates spans, and a database loaded in big transactions has almost
** none to replay.
*/
static void bench_opens(int n){
  char dir[1200];
  char *val = malloc(valsize);
  double t0, dt, best = 1e18, firstOpen = 0.0;
  int r, i;
  sqlite3 *db;

  memset(val, 'v', valsize);
  snprintf(dir, sizeof(dir), "%s/sqlopens", workdir);
  printf("  %-30s%4d", "open+first-read, build/txn", opensPer);
  fflush(stdout);
  {
    /* Build under zBuildUri if given, so "imported cold, read with the
    ** pointer table" is measurable.  Before library 2.4.0 that case did
    ** nothing: a read-only handle would not create the cache directory, so
    ** enabling the flag on an existing database bought nothing until some
    ** unrelated write came along. */
    const char *zSave = zUri;
    if( zBuildUri ) zUri = zBuildUri[0] ? zBuildUri : 0;
    db = open_fresh(dir);
    insert_n(db, val, opensPer);
    sqlite3_close(db);
    zUri = zSave;
  }

  for(r=0; r<reps; r++){
    double tFirst = -1.0;
    t0 = now();
    for(i=0; i<n; i++){
      sqlite3_stmt *pSel;
      char zSql[128];
      sqlite3 *d2;
      int rc;
      if( zUri || deferRepack ){
        char zFile[1400];
        snprintf(zFile, sizeof(zFile), "file:%s?%s", dir, zUri ? zUri : "");
        rc = sqlite3_open_v2(zFile, &d2,
               SQLITE_OPEN_READONLY|SQLITE_OPEN_URI, 0);
      }else{
        rc = sqlite3_open_v2(dir, &d2, SQLITE_OPEN_READONLY, 0);
      }
      if( rc!=SQLITE_OK ){ fprintf(stderr, "reopen failed\n"); exit(1); }
      /* one point read, so the open actually resolves a snapshot */
      snprintf(zSql, sizeof(zSql), bRowid
                 ? "SELECT v FROM kv WHERE k=%d"
                 : "SELECT v FROM kv WHERE k='key%08d'",
               (i*2654435761u) % (unsigned)nrecs);
      if( sqlite3_prepare_v2(d2, zSql, -1, &pSel, 0)!=SQLITE_OK
       || sqlite3_step(pSel)==SQLITE_ERROR ){
        fprintf(stderr, "read after open: %s\n", sqlite3_errmsg(d2));
        exit(1);
      }
      sqlite3_finalize(pSel);
      sqlite3_close(d2);
      if( i==0 ){
        tFirst = now() - t0;
        if( tFirst>firstOpen ) firstOpen = tFirst;   /* worst first open */
      }
    }
    dt = now() - t0;
    if( dt<best ) best = dt;
  }
  cleanup(dir);
  printf("%8.0f/s  %5.2fs  %6.3fms each  (first %6.3fms)\n",
         n/best, best, 1000.0*best/n, 1000.0*firstOpen);
  free(val);
}

int main(int argc, char **argv){
  int i;
  for(i=1; i<argc; i++){
    if( !strcmp(argv[i], "-n") && i+1<argc ) nrecs = atoi(argv[++i]);
    else if( !strcmp(argv[i], "--reps") && i+1<argc ) reps = atoi(argv[++i]);
    else if( !strcmp(argv[i], "--value") && i+1<argc ){
      valsize = (size_t)atol(argv[++i]);
    }else if( !strcmp(argv[i], "--dir") && i+1<argc ){
      snprintf(workdir, sizeof(workdir), "%s", argv[++i]);
    }else if( !strcmp(argv[i], "--init") && i+1<argc ){
      zInit = argv[++i];
    }else if( !strcmp(argv[i], "--uri") && i+1<argc ){
      zUri = argv[++i];
    }else if( !strcmp(argv[i], "--random") ){
      bRandom = 1;
    }else if( !strcmp(argv[i], "--rowid") ){
      bRowid = 1;
    }else if( !strcmp(argv[i], "--vacuum") ){
      doVacuum = 1;
    }else if( !strcmp(argv[i], "--only") && i+1<argc ){
      zOnly = argv[++i];
    }else if( !strcmp(argv[i], "--defer-repack") ){
      deferRepack = 1;
    }else if( !strcmp(argv[i], "--opens") && i+1<argc ){
      nOpens = atoi(argv[++i]);
    }else if( !strcmp(argv[i], "--build-uri") && i+1<argc ){
      zBuildUri = argv[++i];
    }else if( !strcmp(argv[i], "--opens-per") && i+1<argc ){
      opensPer = atoi(argv[++i]);
    }else{
      fprintf(stderr, "usage: zskvbench [--dir PATH] [-n N] [--value N]"
                      " [--reps N] [--init SQL] [--uri PARAMS] [--vacuum]"
                      " [--only N] [--defer-repack] [--opens N]"
                      " [--build-uri PARAMS] [--opens-per N]\n");
      return 2;
    }
  }
  /* Which storage engine did we actually link?  make cannot see that
  ** OPTIONS changed, so a stale libsqlite3.a silently gives you the
  ** btree when you asked for zeroskip -- and the numbers then look
  ** plausible instead of wrong.  Say it out loud on every run.
  **
  ** Same reason for the sanitizer marker: an ASan build left over from
  ** a verification round is 3-4x slower and reads as an ordinary result.
  ** One was taken for a baseline here and produced a 3.8x "speedup" that
  ** did not exist. */
  printf("zskvbench: %d records, %zu-byte values, best of %d (%s) engine=%s"
         " table=%s keys=%s%s\n",
         nrecs, valsize, reps, sqlite3_libversion(),
         sqlite3_compileoption_used("ZEROSKIP") ? "zeroskip" : "btree",
         bRowid ? "rowid" : "withoutrowid",
         bRandom ? "random" : "ascending", ZSKV_INSTRUMENTED);
  if( zInit ) printf("  init: %s\n", zInit);
  if( zUri ) printf("  uri: %s\n", zUri);
  /* --only N runs just the N-per-txn store row.  At a record count large
  ** enough for the repack cascade to matter -- which is where the rewrite
  ** counters get interesting -- the 1-per-txn row alone is 2 million
  ** commits, so a full sweep is not the way to ask that question. */
  if( nOpens>0 ){
    bench_opens(nOpens);
  }else if( zOnly ){
    bench_store(atoi(zOnly));
  }else{
    bench_store(1);
    bench_store(10);
    bench_store(100);
    bench_store(1000);
    bench_fetch_and_scan();
  }
  return 0;
}
