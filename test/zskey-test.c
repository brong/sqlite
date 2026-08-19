/*
** 2026-08-11
**
** Property test for the zeroskip key codec (src/zskey.c).
**
** For random record pairs A,B over supported types and collations,
** sign(memcmp(enc(A),enc(B))) must equal
** sign(sqlite3VdbeRecordCompare(nA, pA, unpack(B))).
**
** Links against the non-amalgamation libsqlite3.a, which exports the
** internal functions, and must be built with SQLITE_ZEROSKIP defined so
** the struct layouts match.
*/
#include "sqliteInt.h"
#include "vdbeInt.h"
#include "zskey.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define NPAIRS 50000

/* ---------- minimal record serializer ---------- */

typedef struct TVal TVal;
struct TVal {
  int eType;        /* 0=NULL 1=int 2=real 3=text 4=blob */
  i64 i;
  double r;
  const char *z;
  int n;
};

/* Serialize aVal[0..nVal) into aOut, returning the record length.
** Only 1-byte header-size and 1-byte serial types are supported, which
** holds for the small values this test generates. */
static int serialize(TVal *aVal, int nVal, u8 *aOut){
  u8 aTypes[64];
  int nTypes = 0;
  int nHdr;
  u8 *pBody;
  int i;
  for(i=0; i<nVal; i++){
    TVal *v = &aVal[i];
    switch( v->eType ){
      case 0: aTypes[nTypes++] = 0; break;
      case 1: aTypes[nTypes++] = 6; break;
      case 2: aTypes[nTypes++] = 7; break;
      case 3: assert( 13+2*v->n < 128 ); aTypes[nTypes++] = (u8)(13+2*v->n); break;
      case 4: assert( 12+2*v->n < 128 ); aTypes[nTypes++] = (u8)(12+2*v->n); break;
    }
  }
  nHdr = 1 + nTypes;
  assert( nHdr < 128 );
  aOut[0] = (u8)nHdr;
  memcpy(aOut+1, aTypes, nTypes);
  pBody = aOut + nHdr;
  for(i=0; i<nVal; i++){
    TVal *v = &aVal[i];
    if( v->eType==1 ){
      u64 x = (u64)v->i;
      int j;
      for(j=7; j>=0; j--){ pBody[j] = x & 0xff; x >>= 8; }
      pBody += 8;
    }else if( v->eType==2 ){
      u64 x;
      int j;
      memcpy(&x, &v->r, 8);
      for(j=7; j>=0; j--){ pBody[j] = x & 0xff; x >>= 8; }
      pBody += 8;
    }else if( v->eType>=3 ){
      memcpy(pBody, v->z, v->n);
      pBody += v->n;
    }
  }
  return (int)(pBody - aOut);
}

/* ---------- value pools ---------- */

static const i64 aInt[] = {
  0, 1, -1, 2, -2, 10, -10, 99, 100, 101, -100,
  8366271098608253952LL,            /* == (double)8366271098608253588 */
  8366271098608253950LL,            /* NOT equal to that double */
  8366271098608253588LL,
  4611686018427387904LL,            /* 2^62 */
  9007199254740992LL,               /* 2^53 */
  9007199254740993LL,               /* 2^53+1: needs exact decimal */
  9007199254740994LL,
  -9007199254740993LL,
  9223372036854775807LL,            /* INT64_MAX */
  -9223372036854775807LL - 1        /* INT64_MIN */
};

static const double aReal[] = {
  0.0, -0.0, 1.0, -1.0, 1.5, -1.5, 0.5, 0.05, -0.05,
  8366271098608253952.0, 1.8446744073709552e19,
  100.0, 99.5, 1e19, 1.0000000000000002,
  9007199254740992.0, 9007199254740994.0,
  1e300, -1e300, 1e-300, -1e-300,
  3.141592653589793, 2.5e-5
};

static const char *aText[] = {
  "", "a", "A", "ab", "aB", "a b", "abc   ", "abc", "b",
  "zzz", "Zebra", "zebra", "  lead", "a\0b", "\x01", "~~~~"
};
static const int aTextLen[] = {
  0, 1, 1, 2, 2, 3, 6, 3, 1,
  3, 5, 5, 6, 3, 1, 4
};

static const char *aBlob[] = { "", "\x00", "\x00\x01", "\xff", "\x00\xff", "ab" };
static const int aBlobLen[] = { 0, 1, 2, 1, 2, 2 };

static const char *aColl[] = { "BINARY", "NOCASE", "RTRIM" };
static const u8 aFlags[] = {
  0, KEYINFO_ORDER_DESC, KEYINFO_ORDER_BIGNULL,
  KEYINFO_ORDER_DESC|KEYINFO_ORDER_BIGNULL
};

/* xorshift64 PRNG, fixed seed for reproducibility */
static u64 prngState = 0x9E3779B97F4A7C15ULL;
static u64 prng(void){
  u64 x = prngState;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  prngState = x;
  return x;
}

static void randVal(TVal *v){
  switch( prng() % 9 ){
    case 0: v->eType = 0; break;
    case 1: case 2: case 3:
      v->eType = 1;
      v->i = aInt[prng() % ArraySize(aInt)];
      break;
    case 4: case 5:
      v->eType = 2;
      v->r = aReal[prng() % ArraySize(aReal)];
      break;
    case 6: case 7: {
      u64 k = prng() % ArraySize(aText);
      v->eType = 3;
      v->z = aText[k];
      v->n = aTextLen[k];
      break;
    }
    default: {
      u64 k = prng() % ArraySize(aBlob);
      v->eType = 4;
      v->z = aBlob[k];
      v->n = aBlobLen[k];
      break;
    }
  }
}

static int sign(i64 x){ return x<0 ? -1 : (x>0 ? 1 : 0); }

static void hexdump(const char *zLabel, const u8 *a, int n){
  int i;
  fprintf(stderr, "%s:", zLabel);
  for(i=0; i<n; i++) fprintf(stderr, " %02x", a[i]);
  fprintf(stderr, "\n");
}

static void dumpVal(const TVal *v){
  switch( v->eType ){
    case 0: fprintf(stderr, "NULL"); break;
    case 1: fprintf(stderr, "int %lld", (long long)v->i); break;
    case 2: fprintf(stderr, "real %.17g", v->r); break;
    case 3: fprintf(stderr, "text[%d] '%.*s'", v->n, v->n, v->z); break;
    case 4: fprintf(stderr, "blob[%d]", v->n); break;
  }
}

int main(void){
  sqlite3 *db = 0;
  int iPair;
  int nTested = 0;
  u8 aRowidKeys[7][8];
  static const i64 aRowidVals[7] = {
    -9223372036854775807LL - 1, -2, -1, 0, 1, 2, 9223372036854775807LL
  };
  int i, j;

  /* The btree backend is stubbed at this stage of the build, so the
  ** open reports an error; the handle is still valid for collation and
  ** malloc purposes, which is all this test needs. */
  sqlite3_open(":memory:", &db);
  if( db==0 ){
    fprintf(stderr, "could not allocate a database handle\n");
    return 1;
  }
  /* the open error path leaves db->enc unset */
  db->enc = SQLITE_UTF8;

  /* rowid codec: round-trip and order */
  for(i=0; i<7; i++){
    zskeyPutRowid(aRowidKeys[i], aRowidVals[i]);
    if( zskeyGetRowid(aRowidKeys[i])!=aRowidVals[i] ){
      fprintf(stderr, "rowid round-trip failed for %lld\n",
              (long long)aRowidVals[i]);
      return 1;
    }
  }
  for(i=0; i<6; i++){
    if( memcmp(aRowidKeys[i], aRowidKeys[i+1], 8)>=0 ){
      fprintf(stderr, "rowid order failed at %d\n", i);
      return 1;
    }
  }

  for(iPair=0; iPair<NPAIRS; iPair++){
    int nField = 1 + (int)(prng() % 3);
    TVal aValA[3], aValB[3];
    u8 aRecA[256], aRecB[256];
    int nRecA, nRecB;
    KeyInfo *pKI;
    UnpackedRecord *pUR;
    u8 *aEncA = 0, *aEncB = 0;
    int nEncA, nEncB;
    int rcA, rcB;
    int cmpEnc, cmpRec;
    int minLen;

    for(i=0; i<nField; i++){
      randVal(&aValA[i]);
      /* half the time share a prefix or the whole value */
      if( prng()%2 ){
        aValB[i] = aValA[i];
      }else{
        randVal(&aValB[i]);
      }
    }

    pKI = sqlite3KeyInfoAlloc(db, nField, 1);
    if( pKI==0 ){ fprintf(stderr, "KeyInfo alloc failed\n"); return 1; }
    for(i=0; i<nField; i++){
      pKI->aColl[i] = sqlite3FindCollSeq(db, SQLITE_UTF8,
                                         aColl[prng()%3], 0);
      pKI->aSortFlags[i] = aFlags[prng()%4];
    }

    nRecA = serialize(aValA, nField, aRecA);
    nRecB = serialize(aValB, nField, aRecB);

    rcA = zskeyEncodeRecord(pKI, nRecA, aRecA, &aEncA, &nEncA);
    rcB = zskeyEncodeRecord(pKI, nRecB, aRecB, &aEncB, &nEncB);
    if( rcA!=SQLITE_OK || rcB!=SQLITE_OK ){
      fprintf(stderr, "pair %d: encode failed rc=%d/%d\n", iPair, rcA, rcB);
      return 1;
    }

    minLen = nEncA<nEncB ? nEncA : nEncB;
    cmpEnc = memcmp(aEncA, aEncB, minLen);
    if( cmpEnc==0 ) cmpEnc = nEncA - nEncB;
    cmpEnc = sign(cmpEnc);

    pUR = sqlite3VdbeAllocUnpackedRecord(pKI);
    if( pUR==0 ){ fprintf(stderr, "unpack alloc failed\n"); return 1; }
    sqlite3VdbeRecordUnpack(nRecB, aRecB, pUR);
    pUR->default_rc = 0;
    cmpRec = sign(sqlite3VdbeRecordCompare(nRecA, aRecA, pUR));

    if( cmpEnc!=cmpRec ){
      fprintf(stderr, "MISMATCH pair %d: enc=%d rec=%d, %d fields\n",
              iPair, cmpEnc, cmpRec, nField);
      for(i=0; i<nField; i++){
        fprintf(stderr, "  field %d (coll=%s flags=%02x): A=", i,
                pKI->aColl[i] ? pKI->aColl[i]->zName : "?",
                pKI->aSortFlags[i]);
        dumpVal(&aValA[i]);
        fprintf(stderr, "  B=");
        dumpVal(&aValB[i]);
        fprintf(stderr, "\n");
      }
      hexdump("  encA", aEncA, nEncA);
      hexdump("  encB", aEncB, nEncB);
      return 1;
    }

    /* int/real equality must be byte-identical: probe when both sides
    ** are single-field numerics that sqlite says are equal */
    if( cmpRec==0 && nEncA!=nEncB ){
      fprintf(stderr, "pair %d: equal records, different lengths\n", iPair);
      return 1;
    }
    if( cmpRec==0 && memcmp(aEncA, aEncB, nEncA)!=0 ){
      fprintf(stderr, "pair %d: equal records, different bytes\n", iPair);
      return 1;
    }

    sqlite3DbFree(pKI->db, pUR);
    sqlite3KeyInfoUnref(pKI);
    sqlite3_free(aEncA);
    sqlite3_free(aEncB);
    nTested++;
  }

  /* MEM_IntReal encodes as its double value, not its exact integer:
  ** a REAL column holding 8366271098608253588 must equal a CAST of
  ** the same literal to REAL. */
  {
    static const i64 aIR[] = { 1, -7, 8366271098608253588LL,
                               9007199254740993LL, -8366271098608253588LL };
    int k;
    KeyInfo *pKI = sqlite3KeyInfoAlloc(db, 1, 1);
    pKI->aColl[0] = 0;
    pKI->aSortFlags[0] = 0;
    for(k=0; k<(int)ArraySize(aIR); k++){
      UnpackedRecord *pA = sqlite3VdbeAllocUnpackedRecord(pKI);
      UnpackedRecord *pB = sqlite3VdbeAllocUnpackedRecord(pKI);
      u8 *eA, *eB; int nA, nB;
      memset(pA->aMem, 0, sizeof(Mem));
      memset(pB->aMem, 0, sizeof(Mem));
      pA->aMem[0].flags = MEM_IntReal;
      pA->aMem[0].u.i = aIR[k];
      pA->aMem[0].db = db;
      pB->aMem[0].flags = MEM_Real;
      pB->aMem[0].u.r = (double)aIR[k];
      pB->aMem[0].db = db;
      pA->nField = pB->nField = 1;
      if( zskeyEncodeUnpacked(pA, 1, &eA, &nA)!=SQLITE_OK
       || zskeyEncodeUnpacked(pB, 1, &eB, &nB)!=SQLITE_OK
       || nA!=nB || memcmp(eA, eB, nA)!=0 ){
        fprintf(stderr, "IntReal encoding mismatch for %lld\n",
                (long long)aIR[k]);
        return 1;
      }
      sqlite3_free(eA); sqlite3_free(eB);
      sqlite3DbFree(pKI->db, pA);
      sqlite3DbFree(pKI->db, pB);
    }
    sqlite3KeyInfoUnref(pKI);
  }

  /* Belt and braces: int 1 vs real 1.0 must encode identically */
  {
    TVal vi, vr;
    u8 aRec1[64], aRec2[64];
    int n1, n2;
    u8 *e1, *e2; int ne1, ne2;
    KeyInfo *pKI = sqlite3KeyInfoAlloc(db, 1, 1);
    pKI->aColl[0] = 0;
    pKI->aSortFlags[0] = 0;
    vi.eType = 1; vi.i = 1;
    vr.eType = 2; vr.r = 1.0;
    n1 = serialize(&vi, 1, aRec1);
    n2 = serialize(&vr, 1, aRec2);
    if( zskeyEncodeRecord(pKI, n1, aRec1, &e1, &ne1)!=SQLITE_OK
     || zskeyEncodeRecord(pKI, n2, aRec2, &e2, &ne2)!=SQLITE_OK
     || ne1!=ne2 || memcmp(e1, e2, ne1)!=0 ){
      fprintf(stderr, "enc(1) != enc(1.0)\n");
      return 1;
    }
    sqlite3_free(e1); sqlite3_free(e2);
    sqlite3KeyInfoUnref(pKI);
  }

  sqlite3_close(db);
  printf("zskey-test PASS (%d pairs)\n", nTested);
  return 0;
}
