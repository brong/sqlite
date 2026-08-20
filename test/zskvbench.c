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
#include <sys/resource.h>

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

/* Like open_fresh but keeps what is there: no cleanup, no CREATE TABLE. */
static sqlite3 *open_existing(const char *zDir){
  sqlite3 *db;
  int rc;
  if( zUri ){
    char zFile[1400];
    snprintf(zFile, sizeof(zFile), "file:%s?%s", zDir, zUri);
    rc = sqlite3_open_v2(zFile, &db,
           SQLITE_OPEN_READWRITE|SQLITE_OPEN_URI, 0);
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
extern int sqlite3ZsRepackCatchUp(sqlite3*, const char*, int, int*, int*);
extern int sqlite3ZsSeal(sqlite3*, const char*);
static int seal_now(sqlite3 *db){
  return sqlite3ZsSeal(db, 0)==SQLITE_OK;
}
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
  if( sqlite3ZsRepackCatchUp(db, 0, 0, pnMerges, 0)!=SQLITE_OK ){
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
static int seal_now(sqlite3 *db){ (void)db; return 0; }
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

/* Build a database with the cascade disarmed and LEAVE it, for a cold-merge
** measurement whose cache drop has to happen in another process. */
static void bench_build_only(void){
  sqlite3 *db;
  char *val = sqlite3_malloc64((sqlite3_int64)valsize);
  double t0;
  if( val==0 ){ fprintf(stderr, "oom\n"); exit(1); }
  memset(val, 'x', valsize);
  deferRepack = 1;               /* the whole point: leave the merging undone */
  db = open_fresh(workdir);
  t0 = now();
  insert_n(db, val, 1000);
  printf("  %-34s%7.0f/s  %5.2fs  (cascade disarmed, files left unmerged)\n",
         "build", nrecs/(now()-t0), now()-t0);
  sqlite3_close(db);
  sqlite3_free(val);
  /* deliberately NO cleanup: --catchup-only is the other half */
}

/* Time the merge alone, over whatever --build-only left.  Reports faults as
** well as time, because the hint under test acts on faults. */
static void bench_catchup_only(void){
  sqlite3 *db;
  struct rusage r0, r1;
  sqlite3_uint64 aBefore[8], aAfter[8];
  int nMerges = 0, bBehind = 0, bStats;
  double dt;

  db = open_existing(workdir);
  bStats = grab_rewrites(db, aBefore);
  getrusage(RUSAGE_SELF, &r0);
  dt = catch_up(db, &nMerges);
  getrusage(RUSAGE_SELF, &r1);
  printf("  %-34s%5.2fs  %d merges%s\n", "cold catch-up", dt, nMerges,
         bBehind ? " (still behind)" : "");
  printf("  %-34sminor %llu  major %llu\n", "  page faults",
         (unsigned long long)(r1.ru_minflt - r0.ru_minflt),
         (unsigned long long)(r1.ru_majflt - r0.ru_majflt));
  if( bStats && grab_rewrites(db, aAfter) ){
    printf("  %-34s%llu repacks %.0fMB %.0fms; %llu conversions %.0fMB %.0fms\n",
           "  merged in this process",
           (unsigned long long)(aAfter[0]-aBefore[0]),
           (double)(aAfter[2]-aBefore[2])/1e6,
           (double)(aAfter[3]-aBefore[3])/1e6,
           (unsigned long long)(aAfter[4]-aBefore[4]),
           (double)(aAfter[6]-aBefore[6])/1e6,
           (double)(aAfter[7]-aBefore[7])/1e6);
  }
  sqlite3_close(db);
  cleanup(workdir);
}

/* --latency: the per-message shape, reported as a DISTRIBUTION.
**
** Every other store row here is a rate, and a rate cannot answer the question
** a mail server actually has.  Deliveries commit one message at a time, and
** the repack cascade runs synchronously inside whichever write transaction
** happens to trip it -- so one delivery in N pays for a whole generation merge
** while the rest pay nothing.  A mean hides that completely: it is a tail
** problem, and whether to move repacks off the critical path is a decision
** about the tail.
**
** So this times each commit separately and reports percentiles, and it
** ATTRIBUTES the outliers rather than assuming they are repacks: the library's
** repack and conversion counters (A-17) are sampled either side of every
** commit, so a commit that merged is known to have merged.  Without that the
** slow tail could equally be ZFS txg boundaries or the append buffer growing,
** and the fix for each is different.
**
** Pair it with zs_norepack=1 to see the same distribution with the cascade off
** the write path -- that difference is what a background repack would buy, and
** it is the number to have before adding a thread to a process that has none. */
static int nLatency = 0;           /* --latency N: N single-record commits */
/* --build-only / --catchup-only: the merge input has to be COLD.
**
** Library 2.9.0 added a posix_madvise(WILLNEED) per merge input, from our own
** call graph showing 11.6% of a bulk load in page faults under XXH3_hashLong.
** A hint can only help when the pages are NOT resident -- and every fixture in
** this file writes its input and merges it moments later, which is the worst
** possible case for observing one.  On the production box the whole 260MB
** database also fits in ARC many times over, so "run it on ZFS" is not enough
** either.  An A/B across those two libraries on the existing cascade phase
** would measure nothing and would wrongly condemn the change.
**
** So the phases are split across processes, with an external cache drop in
** between: --build-only leaves a database with the cascade disarmed and a pile
** of unmerged files, and --catchup-only opens it and times the merge alone.
** Fault counts are reported alongside the time, from getrusage, because the
** hint acts on faults and a fault count is a much less noisy read on whether
** it did anything than a wall clock on a shared machine. */
static int buildOnly = 0;          /* --build-only: build and leave it */
static int catchupOnly = 0;        /* --catchup-only: merge an existing one */
static int sealEvery = 0;          /* --seal-every N: zs_db_seal from "idle"
                                   ** every N commits, untimed, to price the
                                   ** conversion outlier away */

static int cmp_double(const void *a, const void *b){
  double x = *(const double*)a, y = *(const double*)b;
  return x<y ? -1 : (x>y ? 1 : 0);
}
static double pct(const double *aSorted, int n, double p){
  double idx;
  if( n<=0 ) return 0.0;
  idx = p*(n-1)/100.0;
  return aSorted[(int)(idx+0.5)];
}

static void bench_latency(int n){
  sqlite3 *db;
  sqlite3_stmt *pIns;
  double *aAll, *aRepack, *aPlain;
  sqlite3_uint64 aBefore[8], aAfter[8];
  int i, nRepack = 0, nPlain = 0, bStats;
  double tTotal = 0.0;
  char *val;

  aAll    = sqlite3_malloc64((sqlite3_int64)n*sizeof(double));
  aRepack = sqlite3_malloc64((sqlite3_int64)n*sizeof(double));
  aPlain  = sqlite3_malloc64((sqlite3_int64)n*sizeof(double));
  if( aAll==0 || aRepack==0 || aPlain==0 ){
    fprintf(stderr, "out of memory for %d samples\n", n);
    exit(1);
  }
  /* Hoisted out of the loop deliberately: a malloc and a memset per commit is
  ** noise injected into the exact quantity being measured, and at a p50 of
  ** ~24us it is not a small fraction of it. */
  val = sqlite3_malloc64((sqlite3_int64)valsize);
  if( val==0 ){ fprintf(stderr, "oom\n"); exit(1); }
  memset(val, 'x', valsize);
  cleanup(workdir);
  db = open_fresh(workdir);
  if( sqlite3_prepare_v2(db, "INSERT INTO kv VALUES(?1,?2)", -1, &pIns, 0)
        !=SQLITE_OK ){
    fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
    exit(1);
  }
  bStats = grab_rewrites(db, aBefore);
  perm_reset();
  for(i=0; i<n; i++){
    char k[32];
    int key = bRandom ? perm_next() : i;
    double t0, dt;
    if( bRowid ){
      sqlite3_bind_int64(pIns, 1, (sqlite3_int64)key + 1);
    }else{
      snprintf(k, sizeof(k), "key%08d", key);
      sqlite3_bind_text(pIns, 1, k, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_blob(pIns, 2, val, (int)valsize, SQLITE_STATIC);
    /* One implicit transaction per step: this is the shape being measured,
    ** so no BEGIN/COMMIT around it. */
    t0 = now();
    if( sqlite3_step(pIns)!=SQLITE_DONE ){
      fprintf(stderr, "insert: %s\n", sqlite3_errmsg(db));
      exit(1);
    }
    dt = now() - t0;
    sqlite3_reset(pIns);
    aAll[i] = dt;
    tTotal += dt;
    /* Attribute BEFORE sealing.  The first version of this sealed here and
    ** sampled the counters afterwards, which charged the seal's conversion to
    ** the commit that happened to precede it -- and the result looked
    ** wonderful: 40 "merged" commits with a p50 of 24us, i.e. exactly the
    ** number the fixture was supposed to be proving. */
    if( bStats && grab_rewrites(db, aAfter) ){
      /* aOut[0] repacks, aOut[4] conversions -- either one means this commit
      ** did file-lifecycle work the next one will not. */
      if( aAfter[0]!=aBefore[0] || aAfter[4]!=aBefore[4] ){
        aRepack[nRepack++] = dt;
      }else{
        aPlain[nPlain++] = dt;
      }
      memcpy(aBefore, aAfter, sizeof(aBefore));
    }else{
      aPlain[nPlain++] = dt;
    }
    /* The idle moment, simulated: untimed, and after attribution, because the
    ** whole proposition is that the work still happens and merely stops
    ** happening on a commit a user is waiting for.  Re-baseline afterwards so
    ** the seal is not charged to the NEXT commit either. */
    if( sealEvery>0 && (i % sealEvery)==(sealEvery-1) ){
      seal_now(db);
      if( bStats ) grab_rewrites(db, aBefore);
    }
  }
  sqlite3_finalize(pIns);

  qsort(aAll, n, sizeof(double), cmp_double);
  printf("  %-34s%7.0f/s  %5.2fs\n", "commit, one record each",
         n/tTotal, tTotal);
  printf("  %-34sp50 %7.3f  p90 %7.3f  p99 %7.3f  p99.9 %7.3f  max %7.3f ms\n",
         "latency", pct(aAll,n,50)*1e3, pct(aAll,n,90)*1e3,
         pct(aAll,n,99)*1e3, pct(aAll,n,99.9)*1e3, aAll[n-1]*1e3);
  if( nRepack>0 || nPlain>0 ){
    qsort(aRepack, nRepack, sizeof(double), cmp_double);
    qsort(aPlain, nPlain, sizeof(double), cmp_double);
    /* The split is the point: if the tail is NOT the merging commits, moving
    ** repacks off the write path will not fix it. */
    if( nRepack>0 ){
      double tRepack = 0.0;
      int j;
      for(j=0; j<nRepack; j++) tRepack += aRepack[j];
      /* The share of total time is what decides whether backgrounding is
      ** worth anything: a 0.5%% of commits holding 30%% of the time is a tail
      ** worth moving, the same 0.5%% holding 1%% is not. */
      printf("  %-34s%d of %d commits (%.2f%%)  p50 %7.3f  max %7.3f ms"
             "  = %.0f%% of total time\n",
             "  merged", nRepack, n, 100.0*nRepack/n,
             pct(aRepack,nRepack,50)*1e3, aRepack[nRepack-1]*1e3,
             tTotal>0 ? 100.0*tRepack/tTotal : 0.0);
    }else{
      printf("  %-34snone -- the tail is not merging, look elsewhere\n",
             "  merged");
    }
    if( nPlain>0 ){
      printf("  %-34s%d commits            p50 %7.3f  p99 %7.3f  max %7.3f ms\n",
             "  did not merge", nPlain, pct(aPlain,nPlain,50)*1e3,
             pct(aPlain,nPlain,99)*1e3, aPlain[nPlain-1]*1e3);
    }
  }
  if( bStats && grab_rewrites(db, aAfter) ){
    print_rewrites(aAfter, (double)n * (double)(11 + valsize));
  }
  sqlite3_close(db);
  cleanup(workdir);
  sqlite3_free(val);
  sqlite3_free(aAll); sqlite3_free(aRepack); sqlite3_free(aPlain);
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
    }else if( !strcmp(argv[i], "--build-only") ){
      buildOnly = 1;
    }else if( !strcmp(argv[i], "--catchup-only") ){
      catchupOnly = 1;
    }else if( !strcmp(argv[i], "--seal-every") && i+1<argc ){
      sealEvery = atoi(argv[++i]);
    }else if( !strcmp(argv[i], "--latency") && i+1<argc ){
      nLatency = atoi(argv[++i]);
      reps = 1;                  /* a distribution over one pass; "best of N"
                                 ** is meaningless here and the header would
                                 ** otherwise say it */
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
                      " [--rowid] [--random] [--only N] [--defer-repack]"
                      " [--opens N] [--build-uri PARAMS] [--opens-per N]"
                      " [--latency N] [--seal-every N]"
                      " [--build-only] [--catchup-only]\n");
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
  if( buildOnly ){
    bench_build_only();
  }else if( catchupOnly ){
    bench_catchup_only();
  }else if( nLatency>0 ){
    bench_latency(nLatency);
  }else if( nOpens>0 ){
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
