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
** Order-preserving key encoding for the zeroskip storage engine.
** The format is specified in zskey.h; the property test in
** test/zskey-test.c holds it to sqlite3VdbeRecordCompare as ground
** truth.
*/
#ifdef SQLITE_ZEROSKIP
#include "sqliteInt.h"
#include "vdbeInt.h"
#include "zskey.h"
#include <math.h>
#include <stdio.h>    /* C-library snprintf: %.17e must round-trip doubles,
                      ** which sqlite3_snprintf's approximate %e does not */

void zskeyPutTreeId(u8 *a, u32 iTree){
  a[0] = (u8)(iTree>>24);
  a[1] = (u8)(iTree>>16);
  a[2] = (u8)(iTree>>8);
  a[3] = (u8)iTree;
}
u32 zskeyGetTreeId(const u8 *a){
  return ((u32)a[0]<<24) | ((u32)a[1]<<16) | ((u32)a[2]<<8) | (u32)a[3];
}

void zskeyPutRowid(u8 *a, i64 rowid){
  u64 x = (u64)rowid ^ 0x8000000000000000ULL;   /* sign flip */
  int i;
  for(i=7; i>=0; i--){ a[i] = (u8)(x & 0xff); x >>= 8; }
}
i64 zskeyGetRowid(const u8 *a){
  u64 x = 0;
  int i;
  for(i=0; i<8; i++) x = (x<<8) | a[i];
  return (i64)(x ^ 0x8000000000000000ULL);
}

/*
** Growable output buffer.  rc goes sticky on OOM or an unsupported
** collation/encoding; append calls after that are no-ops.
*/
typedef struct ZsKeyOut ZsKeyOut;
struct ZsKeyOut {
  u8 *a;
  int n;
  int nAlloc;
  int rc;
};

static int outGrow(ZsKeyOut *o, int nNeed){
  int nNew = o->nAlloc ? o->nAlloc*2 : 64;
  u8 *aNew;
  while( nNew<o->n+nNeed ) nNew *= 2;
  aNew = sqlite3Realloc(o->a, nNew);
  if( aNew==0 ){ o->rc = SQLITE_NOMEM; return 0; }
  o->a = aNew;
  o->nAlloc = nNew;
  return 1;
}

static void outByte(ZsKeyOut *o, u8 c){
  if( o->rc ) return;
  if( o->n>=o->nAlloc && !outGrow(o, 1) ) return;
  o->a[o->n++] = c;
}

static void outBytes(ZsKeyOut *o, const u8 *a, int n){
  if( o->rc ) return;
  if( o->n+n>o->nAlloc && !outGrow(o, n) ) return;
  memcpy(o->a+o->n, a, n);
  o->n += n;
}

/*
** Append the magnitude encoding of 0.zDigits * 10^e (zDigits has no
** leading or trailing zeros).  A virtual leading zero digit is used
** when e is odd, so the digit string itself is never moved.
*/
static void outMagnitude(ZsKeyOut *o, const char *zDigits, int nDigits, int e){
  int pad = (e & 1) ? 1 : 0;
  int nVirtual = pad + nDigits;
  int nPairs = (nVirtual + 1) / 2;
  int E;
  int i;
  e += pad;
  E = e/2 + 200;
  assert( E>=0 && E<=0xffff );
  outByte(o, (u8)(E>>8));
  outByte(o, (u8)(E & 0xff));
  for(i=0; i<nPairs; i++){
    int k0 = 2*i;         /* virtual digit positions */
    int k1 = 2*i + 1;
    int d0 = (k0>=pad && k0-pad<nDigits) ? zDigits[k0-pad]-'0' : 0;
    int d1 = (k1>=pad && k1-pad<nDigits) ? zDigits[k1-pad]-'0' : 0;
    int D = d0*10 + d1;
    outByte(o, (u8)(i==nPairs-1 ? 2*D : 2*D+1));
  }
}

/*
** Decimal digits of an exact 64-bit magnitude.  Returns the exponent e
** (the full digit count); *pnDigits gets the count after stripping
** trailing zeros.
*/
static int digitsFromU64(u64 mag, char *zBuf, int *pnDigits){
  int e, n;
  assert( mag>0 );
  sqlite3_snprintf(24, zBuf, "%llu", (unsigned long long)mag);
  e = n = sqlite3Strlen30(zBuf);
  while( n>0 && zBuf[n-1]=='0' ) n--;
  *pnDigits = n;
  return e;
}

/*
** Decimal digits of a finite nonzero double magnitude, via round-trip
** %.17e formatting.
*/
static int digitsFromDouble(double r, char *zBuf, int *pnDigits){
  char zNum[40];
  char *zExp;
  int n = 0;
  int e10;
  int i;
  assert( r>0.0 );
  if( r>=9007199254740992.0 ){
    /* Every double >= 2^53 is integral; %.17e rounds its exact decimal
    ** value, so an integer Mem equal to it would encode differently
    ** (intreal-2.5 via numeric affinity).  %.0f prints the exact
    ** integral value. */
    char zBig[352];
    int e;
    snprintf(zBig, sizeof(zBig), "%.0f", r);
    e = n = (int)strlen(zBig);
    assert( n<340 );
    while( n>0 && zBig[n-1]=='0' ) n--;
    memcpy(zBuf, zBig, n);
    *pnDigits = n;
    return e;
  }
  snprintf(zNum, sizeof(zNum), "%.17e", r);
  /* "d.ddddddddddddddddde[+-]dd" */
  for(i=0; zNum[i] && zNum[i]!='e' && zNum[i]!='E'; i++){
    if( zNum[i]>='0' && zNum[i]<='9' ) zBuf[n++] = zNum[i];
  }
  zExp = &zNum[i];
  assert( *zExp=='e' || *zExp=='E' );
  e10 = sqlite3Atoi(zExp+1);
  while( n>0 && zBuf[n-1]=='0' ) n--;
  /* value = d.frac * 10^e10 = 0.digits * 10^(e10+1) */
  *pnDigits = n;
  return e10 + 1;
}

/* Field type bytes */
#define ZSKEY_NULL     0x05
#define ZSKEY_NEG_INF  0x07
#define ZSKEY_NEG      0x08
#define ZSKEY_ZERO     0x15
#define ZSKEY_POS      0x22
#define ZSKEY_POS_INF  0x23
#define ZSKEY_TEXT     0x24
#define ZSKEY_BLOB     0x34
#define ZSKEY_NULL_BIG 0xFA

/* Emit escaped bytes and the terminator: 0x00 -> 0x00 0x01, then 0x00 0x00 */
static void outEscaped(ZsKeyOut *o, const u8 *z, int n, int nocase){
  if( nocase ){
    int i;
    for(i=0; i<n; i++){
      u8 c = sqlite3UpperToLower[z[i]];
      outByte(o, c);
      if( c==0 ) outByte(o, 0x01);
    }
  }else{
    /* NUL bytes are rare in keys: bulk-copy between them */
    int i = 0;
    while( i<n ){
      const u8 *pNul = memchr(z+i, 0, n-i);
      int nChunk = pNul ? (int)(pNul-(z+i)) : n-i;
      outBytes(o, z+i, nChunk);
      i += nChunk;
      if( pNul ){
        outByte(o, 0x00);
        outByte(o, 0x01);
        i++;
      }
    }
  }
  outByte(o, 0x00);
  outByte(o, 0x00);
}

static void outNumeric(ZsKeyOut *o, const char *zDigits, int nDigits,
                       int e, int neg){
  int start;
  outByte(o, neg ? ZSKEY_NEG : ZSKEY_POS);
  start = o->n;
  outMagnitude(o, zDigits, nDigits, e);
  if( neg && o->rc==SQLITE_OK ){
    int i;
    for(i=start; i<o->n; i++) o->a[i] = (u8)~o->a[i];
  }
}

static void zskeyEncodeMem(
  ZsKeyOut *o,
  Mem *pMem,
  u8 sortFlags,
  CollSeq *pColl
){
  int start = o->n;
  int flags = pMem->flags;
  char zDigits[352];
  int nDigits;
  int e;

  if( o->rc ) return;
  if( flags & MEM_Null ){
    outByte(o, (sortFlags & KEYINFO_ORDER_BIGNULL) ? ZSKEY_NULL_BIG
                                                   : ZSKEY_NULL);
  }else if( (flags & MEM_Int)!=0 && (flags & MEM_IntReal)==0 ){
    i64 v = pMem->u.i;
    if( v==0 ){
      outByte(o, ZSKEY_ZERO);
    }else{
      u64 mag = v<0 ? ~(u64)v + 1 : (u64)v;
      e = digitsFromU64(mag, zDigits, &nDigits);
      outNumeric(o, zDigits, nDigits, e, v<0);
    }
  }else if( flags & (MEM_Real|MEM_IntReal) ){
    /* MEM_IntReal is a REAL stored compactly (it may carry MEM_Int
    ** too): record comparison treats it as (double)u.i, so the
    ** encoding must too (intreal-2.5) */
    double r = (flags & MEM_IntReal) ? (double)pMem->u.i : pMem->u.r;
    if( r==0.0 ){
      outByte(o, ZSKEY_ZERO);
    }else if( isnan(r) ){
      /* A serialized NaN unpacks as NULL (see serialGet in vdbeaux.c);
      ** match that if one arrives via an in-memory Mem. */
      outByte(o, (sortFlags & KEYINFO_ORDER_BIGNULL) ? ZSKEY_NULL_BIG
                                                     : ZSKEY_NULL);
    }else if( isinf(r) ){
      outByte(o, r<0 ? ZSKEY_NEG_INF : ZSKEY_POS_INF);
    }else{
      e = digitsFromDouble(r<0 ? -r : r, zDigits, &nDigits);
      outNumeric(o, zDigits, nDigits, e, r<0);
    }
  }else if( flags & MEM_Str ){
    int nocase = 0;
    int n = pMem->n;
    if( pMem->enc!=SQLITE_UTF8 ){
      o->rc = SQLITE_ERROR;
      return;
    }
    if( pColl==0 || strcmp(pColl->zName, "BINARY")==0 ){
      /* no transform */
    }else if( strcmp(pColl->zName, "NOCASE")==0 ){
      nocase = 1;
    }else if( strcmp(pColl->zName, "RTRIM")==0 ){
      while( n>0 && pMem->z[n-1]==' ' ) n--;
    }else{
      o->rc = SQLITE_ERROR;
      return;
    }
    outByte(o, ZSKEY_TEXT);
    outEscaped(o, (const u8*)pMem->z, n, nocase);
  }else{
    assert( flags & MEM_Blob );
    if( flags & MEM_Zero ){
      if( sqlite3VdbeMemExpandBlob(pMem)!=SQLITE_OK ){
        o->rc = SQLITE_NOMEM;
        return;
      }
    }
    outByte(o, ZSKEY_BLOB);
    outEscaped(o, (const u8*)pMem->z, pMem->n, 0);
  }

  if( (sortFlags & KEYINFO_ORDER_DESC)!=0 && o->rc==SQLITE_OK ){
    int i;
    for(i=start; i<o->n; i++) o->a[i] = (u8)~o->a[i];
  }
}

int zskeyBufAppend(ZsKeyBuf *pBuf, const u8 *a, int n){
  ZsKeyOut o;
  o.a = pBuf->a;
  o.n = pBuf->n;
  o.nAlloc = pBuf->nAlloc;
  o.rc = SQLITE_OK;
  outBytes(&o, a, n);
  pBuf->a = o.a;
  pBuf->nAlloc = o.nAlloc;
  if( o.rc==SQLITE_OK ) pBuf->n = o.n;
  return o.rc;
}

int zskeyEncodeUnpackedBuf(
  UnpackedRecord *pRec,
  int nField,
  ZsKeyBuf *pBuf
){
  ZsKeyOut o;
  KeyInfo *pKI = pRec->pKeyInfo;
  int i;

  if( pKI->enc!=SQLITE_UTF8 ){
    return SQLITE_ERROR;
  }
  o.a = pBuf->a;
  o.n = pBuf->n;
  o.nAlloc = pBuf->nAlloc;
  o.rc = SQLITE_OK;
  for(i=0; i<nField && o.rc==SQLITE_OK; i++){
    u8 sf = (i < pKI->nKeyField) ? pKI->aSortFlags[i] : 0;
    CollSeq *pColl = (i < pKI->nKeyField) ? pKI->aColl[i] : 0;
    zskeyEncodeMem(&o, &pRec->aMem[i], sf, pColl);
  }
  pBuf->a = o.a;
  pBuf->nAlloc = o.nAlloc;
  if( o.rc ) return o.rc;
  pBuf->n = o.n;
  return SQLITE_OK;
}

int zskeyEncodeRecordBuf(
  KeyInfo *pKeyInfo,
  int nRec,
  const void *pRec,
  UnpackedRecord **ppScratch,
  ZsKeyBuf *pBuf
){
  UnpackedRecord *p = *ppScratch;

  if( p==0 ){
    p = sqlite3VdbeAllocUnpackedRecord(pKeyInfo);
    if( p==0 ) return SQLITE_NOMEM;
    *ppScratch = p;
  }
  sqlite3VdbeRecordUnpack(nRec, pRec, p);
  return zskeyEncodeUnpackedBuf(p, p->nField, pBuf);
}

int zskeyEncodeUnpacked(
  UnpackedRecord *pRec,
  int nField,
  u8 **pzOut,
  int *pnOut
){
  ZsKeyBuf buf = {0, 0, 0};
  int rc = zskeyEncodeUnpackedBuf(pRec, nField, &buf);

  if( rc==SQLITE_OK && buf.a==0 ){
    buf.a = sqlite3Malloc(1);         /* zero-field record: empty key */
    if( buf.a==0 ) rc = SQLITE_NOMEM;
  }
  if( rc ){
    sqlite3_free(buf.a);
    *pzOut = 0;
    *pnOut = 0;
    return rc;
  }
  *pzOut = buf.a;
  *pnOut = buf.n;
  return SQLITE_OK;
}

int zskeyEncodeRecord(
  KeyInfo *pKeyInfo,
  int nRec,
  const void *pRec,
  u8 **pzOut,
  int *pnOut
){
  UnpackedRecord *p = 0;
  ZsKeyBuf buf = {0, 0, 0};
  int rc = zskeyEncodeRecordBuf(pKeyInfo, nRec, pRec, &p, &buf);

  sqlite3DbFree(pKeyInfo->db, p);
  if( rc==SQLITE_OK && buf.a==0 ){
    buf.a = sqlite3Malloc(1);
    if( buf.a==0 ) rc = SQLITE_NOMEM;
  }
  if( rc ){
    sqlite3_free(buf.a);
    *pzOut = 0;
    *pnOut = 0;
    return rc;
  }
  *pzOut = buf.a;
  *pnOut = buf.n;
  return SQLITE_OK;
}

#endif /* SQLITE_ZEROSKIP */
