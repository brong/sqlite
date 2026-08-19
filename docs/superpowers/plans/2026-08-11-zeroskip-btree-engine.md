# Zeroskip Storage Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace SQLite's btree layer with a zeroskip-backed engine (compile-time swap), per the approved spec at `docs/superpowers/specs/2026-08-11-zeroskip-btree-engine-design.md`.

**Architecture:** New `src/btree_zs.c` implements all of `btree.h` on one zeroskip database (byte-order comparator, `[4-byte tree-id][encoded key]` keys, memcmp-encodable index keys, values = original records). `src/backup_zs.c` reimplements the online backup API over zeroskip snapshots. Stock `btree.c`/`backup.c` are compiled out via `#ifndef SQLITE_ZEROSKIP` guards; everything else (pager included, as a vestigial in-memory pager per Btree) stays.

**Tech Stack:** C (SQLite house style: 2-space indent, `/* */` comments, `u8/u32/i64` typedefs), zeroskip vendored from `../zeroskip2`, autosetup `main.mk` build, bash+diff SQL test harness.

## Global Constraints

- All new engine code is inside `#ifdef SQLITE_ZEROSKIP` (whole-file guards); the stock build must be byte-for-byte unaffected apart from the two 2-line `#ifndef SQLITE_ZEROSKIP` wrappers.
- `SQLITE_ZEROSKIP` requires `SQLITE_OMIT_SHARED_CACHE`; enforce with `#error` in btree_zs.c.
- Non-amalgamation builds only. Canonical build command (from repo root, after `./configure`):
  `make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' lib sqlite3zs`
- UTF-8 databases only: index-key encoding rejects `KeyInfo.enc != SQLITE_UTF8` with `SQLITE_ERROR`.
- Supported collations: BINARY, NOCASE, RTRIM (by `CollSeq.zName` strcmp). Anything else → `SQLITE_ERROR` from the encoder.
- Error mapping helper (single function, used everywhere): `ZS_LOCKED→SQLITE_BUSY`, `ZS_READONLY→SQLITE_READONLY`, `ZS_BADFORMAT/ZS_BADCHECKSUM→SQLITE_CORRUPT`, `ZS_FULL→SQLITE_FULL`, `ZS_IOERROR→SQLITE_IOERR`, `ZS_NOTFOUND` handled at call sites (not an error), else `SQLITE_INTERNAL`.
- Never let a zeroskip pointer outlive its producing txn/cursor (spec rule A-4). The only heap copies of positions happen at read→write upgrade and cursor trip.
- Commit after every green test cycle. Branch: `zeroskip-engine`.

---

### Task 1: Vendor zeroskip

**Files:**
- Create: `ext/zeroskip/zeroskip.c`, `ext/zeroskip/zeroskip.h`, `ext/zeroskip/xxhash.h`, `ext/zeroskip/VENDOR`
- Modify: `main.mk:522` (LIBOBJS0), plus a compile rule near `main.mk:1188` (btree.o's rule)

**Interfaces:**
- Produces: `zeroskip.o` in `libsqlite3.a`; header at `ext/zeroskip/zeroskip.h` for later tasks.

- [ ] **Step 1: Copy the vendored files and record provenance**

```bash
cp ../zeroskip2/zeroskip.c ../zeroskip2/zeroskip.h ../zeroskip2/xxhash.h ext/zeroskip/
( cd ../zeroskip2 && echo "source: $(git rev-parse HEAD 2>/dev/null || echo unversioned)" ) > ext/zeroskip/VENDOR
echo "date: 2026-08-11" >> ext/zeroskip/VENDOR
echo "files: zeroskip.c zeroskip.h xxhash.h" >> ext/zeroskip/VENDOR
```

- [ ] **Step 2: Verify reverse-iteration support is present**

Run: `grep -c "ZS_REVERSE\|ZS_FETCHPREV\|reverse" ext/zeroskip/zeroskip.h`

Expected: nonzero — the zeroskip2 reverse-iteration work was handed off and should be done. Record the actual flag spellings in `ext/zeroskip/VENDOR` (e.g. `reverse-api: ZS_REVERSE ZS_FETCHPREV`); Task 7 reads them from there. If absent, note `reverse-api: MISSING` and continue — Tasks 2–6 don't need it, Task 7 blocks on a re-vendor.

- [ ] **Step 3: Add to the build**

In `main.mk`, append `zeroskip.o` to the `LIBOBJS0` list (line ~522, alphabetical position at the end is fine), and add a compile rule following the pattern of the `btree.o` rule at line ~1188:

```make
zeroskip.o:	$(TOP)/ext/zeroskip/zeroskip.c $(TOP)/ext/zeroskip/zeroskip.h
	$(T.cc.sqlite) -std=c99 -I$(TOP)/ext/zeroskip -c $(TOP)/ext/zeroskip/zeroskip.c
```

(zeroskip is C99; `T.cc.sqlite` inherits the configured compiler flags. If `-D_HAVE_SQLITE_CONFIG_H` in `T.cc.sqlite.extras` upsets zeroskip.c — it shouldn't, it doesn't include sqlite headers — fall back to `$(T.compile)`.)

- [ ] **Step 4: Build and verify**

Run: `./configure >/dev/null && make USE_AMALGAMATION=0 lib 2>&1 | tail -5 && nm libsqlite3.a 2>/dev/null | grep -c zs_db_open`

Expected: build succeeds, count ≥ 1.

- [ ] **Step 5: Verify the stock build is unaffected**

Run: `make sqlite3 >/dev/null 2>&1 && ./sqlite3 :memory: 'select 1;'`

Expected: `1`

- [ ] **Step 6: Commit**

```bash
git add ext/zeroskip main.mk
git commit -m "Vendor zeroskip into ext/zeroskip and add to the build"
```

---

### Task 2: Compile-time swap with a stub engine

**Files:**
- Modify: `src/btree.c` (2-line guard), `src/backup.c` (2-line guard), `main.mk` (add `btree_zs.o`, `backup_zs.o`, `sqlite3zs` shell target)
- Create: `src/btree_zs.c`, `src/backup_zs.c`

**Interfaces:**
- Produces: every function in `btree.h` and the `sqlite3_backup*` API links under `SQLITE_ZEROSKIP`; `struct Btree`/`struct BtCursor` defined in btree_zs.c (opaque elsewhere); `sqlite3zs` shell binary target.
- Produces for later tasks: `static int zsbtErr(int zsrc)` (zeroskip→SQLite error mapping per Global Constraints).

- [ ] **Step 1: Guard the stock files**

At the top of `src/btree.c`, immediately after the header comment block (before `#include "btreeInt.h"`), add:

```c
#ifndef SQLITE_ZEROSKIP
```

and at the very end of the file:

```c
#endif /* !defined(SQLITE_ZEROSKIP) */
```

Same two lines in `src/backup.c`.

- [ ] **Step 2: Create btree_zs.c with structs and stubs**

`src/btree_zs.c` skeleton — the file opens:

```c
/*
** 2026-08-11
**
** Zeroskip storage engine: implements the btree.h interface on top of
** the zeroskip append-only key-value store (ext/zeroskip).  Compiled in
** place of btree.c when SQLITE_ZEROSKIP is defined.
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

/* Key-space layout: every zeroskip key is [4-byte BE tree-id][payload].
** Tree-id 0 is meta: [0,0,0,0,'M',slot] for the 16 meta slots,
** [0,0,0,0,'T'] for the tree-id allocation counter.
** Tree-id 1 is sqlite_schema, matching SQLITE's root-page convention. */

typedef struct ZsUndo ZsUndo;
struct ZsUndo {                 /* filled in by the savepoint task */
  int nMark;                    /* number of open savepoint marks */
};

struct Btree {
  sqlite3 *db;                  /* database connection holding this btree */
  struct zs_db *pZs;            /* zeroskip handle, 0 until first use */
  struct zs_txn *pTxn;          /* open txn or 0 */
  int eTxn;                     /* TRANS_NONE, TRANS_READ or TRANS_WRITE */
  char *zDir;                   /* database directory (malloc'd) */
  u8 isEphemeral;               /* delete zDir recursively on close */
  u8 inBackup;                  /* sqlite3BtreeIsInBackup */
  Pager *pFakePager;            /* vestigial in-memory pager */
  Schema *pSchema;              /* schema object, owned */
  void (*xFreeSchema)(void*);   /* destructor for pSchema */
  BtCursor *pCursor;            /* list of open cursors */
  u64 writeEpoch;               /* bumped on every write; cursors re-seek */
  ZsUndo undo;                  /* savepoint undo log */
  u32 iDataVersion;             /* BTREE_DATA_VERSION surrogate */
};

/* Cursor states */
#define ZS_CUR_INVALID     0    /* no position */
#define ZS_CUR_VALID       1    /* borrowed position is current */
#define ZS_CUR_REQUIRESEEK 2    /* position saved in aSavedKey, re-seek */
#define ZS_CUR_FAULT       3    /* return skipNext as error code */

struct BtCursor {
  Btree *pBtree;
  u32 iTree;
  u8 aPrefix[4];                /* big-endian iTree, the key prefix */
  struct zs_cursor *pZc;        /* open zeroskip cursor or 0 */
  u8 eState;
  u8 curIntKey;                 /* true for BTREE_INTKEY trees */
  u8 wrFlag;
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
  BtCursor *pNext;              /* next cursor on the same Btree */
};

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
```

Then one stub per `btree.h` function. Stub behaviors (implement exactly; later tasks replace the bodies):

| Function(s) | Stub body |
|---|---|
| `sqlite3BtreeOpen` | `return SQLITE_INTERNAL;` |
| `sqlite3BtreeClose` | `return SQLITE_OK;` |
| `sqlite3BtreeSetCacheSize/SetSpillSize/SetPagerFlags/SetMmapLimit` | `return SQLITE_OK;` |
| `sqlite3BtreeSetPageSize` | `return SQLITE_READONLY;` (stock's fixed-page answer) |
| `sqlite3BtreeGetPageSize` | `return 4096;` |
| `sqlite3BtreeMaxPageCount/LastPage` | `return 0x7fffffff;` / `return 1;` |
| `sqlite3BtreeSecureDelete` | `return 0;` |
| `sqlite3BtreeGetRequestedReserve/GetReserveNoMutex` | `return 0;` |
| `sqlite3BtreeSetAutoVacuum` | `return SQLITE_READONLY;` |
| `sqlite3BtreeGetAutoVacuum` | `return BTREE_AUTOVACUUM_NONE;` |
| `sqlite3BtreeBeginTrans/CommitPhaseOne/CommitPhaseTwo/Commit/Rollback/BeginStmt/Savepoint` | `return SQLITE_INTERNAL;` |
| `sqlite3BtreeCreateTable/DropTable/ClearTable/ClearTableOfCursor/NewDb` | `return SQLITE_INTERNAL;` |
| `sqlite3BtreeTxnState` | `return p ? p->eTxn : SQLITE_TXN_NONE;` |
| `sqlite3BtreeIsInBackup` | `return p->inBackup;` |
| `sqlite3BtreeSchema` | store/create via the callback args, mirror stock: allocate `pSchema` with `sqlite3DbMallocZero(0, nBytes)` on first call, save `xFreeSchema`, return it |
| `sqlite3BtreeSchemaLocked` | `return SQLITE_OK;` |
| `sqlite3BtreeCheckpoint` | `return SQLITE_OK;` |
| `sqlite3BtreeGetFilename` | `return p->zDir ? p->zDir : "";` |
| `sqlite3BtreeGetJournalname` | `return 0;` |
| `sqlite3BtreeIncrVacuum` | `return SQLITE_DONE;` |
| `sqlite3BtreeTripAllCursors` | `return SQLITE_OK;` |
| `sqlite3BtreeGetMeta` | `*pValue = 0;` (returns void) |
| `sqlite3BtreeUpdateMeta` | `return SQLITE_INTERNAL;` |
| `sqlite3BtreeCursor` | `return SQLITE_INTERNAL;` |
| `sqlite3BtreeFakeValidCursor` | `static u8 fake[sizeof(BtCursor)]; return (BtCursor*)fake;` with `eState==ZS_CUR_INVALID` (zeroed = INVALID) |
| `sqlite3BtreeCursorSize` | `return ROUND8(sizeof(BtCursor));` |
| `sqlite3BtreeCursorZero` | `memset(p, 0, sizeof(BtCursor));` |
| `sqlite3BtreeCursorHintFlags` | `pCur->hints = x;` |
| `sqlite3BtreeCloseCursor` | `return SQLITE_OK;` |
| `sqlite3BtreeTableMoveto/IndexMoveto/First/Last/Next/Previous/Insert/Delete/TransferRow` | `return SQLITE_INTERNAL;` |
| `sqlite3BtreeCursorHasMoved` | `return pCur->eState != ZS_CUR_VALID;` |
| `sqlite3BtreeCursorRestore` | `return SQLITE_INTERNAL;` |
| `sqlite3BtreeEof` | `return pCur->eState != ZS_CUR_VALID;` |
| `sqlite3BtreeIsEmpty` | `return SQLITE_INTERNAL;` |
| `sqlite3BtreeIntegerKey` | `return pCur->intKey;` |
| `sqlite3BtreeCursorPin/Unpin` | no-op |
| `sqlite3BtreeOffset` | `return 0;` |
| `sqlite3BtreePayload/PayloadFetch/PayloadSize` | `return SQLITE_INTERNAL;` / `*pAmt=0; return 0;` / `return 0;` |
| `sqlite3BtreeMaxRecordSize` | `return pCur->pBtree->db->aLimit[SQLITE_LIMIT_LENGTH];` |
| `sqlite3BtreeIntegrityCheck` | `*pnErr = 0; return SQLITE_OK;` |
| `sqlite3BtreePager` | `return p->pFakePager;` |
| `sqlite3BtreeRowCountEst` | `return 1000000;` |
| `sqlite3BtreePayloadChecked/PutData` | `return SQLITE_ERROR;` (incrblob unsupported) |
| `sqlite3BtreeIncrblobCursor` | no-op |
| `sqlite3BtreeClearCursor` | `pCur->eState = ZS_CUR_INVALID;` |
| `sqlite3BtreeSetVersion` | `return SQLITE_OK;` |
| `sqlite3BtreeCursorHasHint` | `return (pCur->hints & mask) != 0;` |
| `sqlite3BtreeIsReadonly` | `return 0;` |
| `sqlite3HeaderSizeBtree` | `return 0;` |
| `sqlite3BtreeCursorIsValid/IsValidNN` | `return pCur && pCur->eState==ZS_CUR_VALID;` / `return pCur->eState==ZS_CUR_VALID;` |
| `sqlite3BtreeCount` | `return SQLITE_INTERNAL;` |
| `sqlite3BtreeClearCache` | no-op |
| `sqlite3BtreeSeekCount` (SQLITE_DEBUG) | `return 0;` |
| `sqlite3BtreeClosesWithCursor` (SQLITE_DEBUG) | `return 0;` |
| `sqlite3BtreeCursorHint` (ENABLE_CURSOR_HINTS) | no-op |

Wrap the SQLITE_DEBUG/SQLITE_TEST-only ones in the same `#ifdef`s btree.h uses. Do NOT define the `sqlite3BtreeEnter*`/`Leave*` family — `SQLITE_OMIT_SHARED_CACHE` makes them macros.

- [ ] **Step 3: Create backup_zs.c with stubs**

```c
#ifdef SQLITE_ZEROSKIP
#include "sqliteInt.h"
#include "btree.h"

sqlite3_backup *sqlite3_backup_init(
  sqlite3* pDestDb, const char *zDestName,
  sqlite3* pSrcDb, const char *zSrcName
){
  sqlite3_mutex_enter(pDestDb->mutex);
  sqlite3ErrorWithMsg(pDestDb, SQLITE_ERROR, "backup not yet supported");
  sqlite3_mutex_leave(pDestDb->mutex);
  return 0;
}
int sqlite3_backup_step(sqlite3_backup *p, int nPage){ return SQLITE_MISUSE; }
int sqlite3_backup_finish(sqlite3_backup *p){ return SQLITE_OK; }
int sqlite3_backup_remaining(sqlite3_backup *p){ return 0; }
int sqlite3_backup_pagecount(sqlite3_backup *p){ return 0; }
int sqlite3BtreeCopyFile(Btree *pTo, Btree *pFrom){ return SQLITE_ERROR; }
#endif /* SQLITE_ZEROSKIP */
```

- [ ] **Step 4: Wire the build**

In `main.mk`: add `btree_zs.o backup_zs.o` to `LIBOBJS0`; add compile rules mirroring `btree.o`'s (both need `-I$(TOP)/ext/zeroskip` added via the rule); add a shell target that links against the static lib instead of the amalgamation:

```make
sqlite3zs$(T.exe):	shell.c $(libsqlite3.LIB)
	$(T.link) -o $@ shell.c -I. -I$(TOP)/src $(SHELL_OPT) \
		$(libsqlite3.LIB) $(LDFLAGS.libsqlite3)
```

(Check how `sqlite3$(T.exe)` passes shell options and copy its variable usage — same `$(SHELL_OPT)`, same LDFLAGS; only the library source differs.)

- [ ] **Step 5: Build the zeroskip configuration; fix link errors**

Run: `make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' lib sqlite3zs 2>&1 | tail -20`

Expected: complete link of `sqlite3zs`. The linker is the authority on stub completeness: any `sqlite3Btree*` symbol it reports missing (a few exist outside btree.h, e.g. cursor-hint helpers) gets a stub added following the table's spirit (read the stock implementation, return the neutral value). List any such additions in the commit message.

- [ ] **Step 6: Verify both configurations**

```bash
./sqlite3zs /tmp/zs-smoke-db 'select 1;' ; echo "exit=$?"
```

Expected: an error (engine is stubbed — `SQLITE_INTERNAL` or similar), NOT a crash. Then rebuild stock (`make sqlite3 && ./sqlite3 :memory: 'select 1;'` → `1`).

- [ ] **Step 7: Commit**

```bash
git add src/btree.c src/backup.c src/btree_zs.c src/backup_zs.c main.mk
git commit -m "Add SQLITE_ZEROSKIP compile-time swap with stub engine"
```

---

### Task 3: Key codec with property tests

**Files:**
- Create: `src/zskey.c`, `src/zskey.h`, `test/zskey-test.c`
- Modify: `main.mk` (add `zskey.o` to LIBOBJS0 + rule; add `zskey-test` target)

**Interfaces:**
- Produces (in `src/zskey.h`, all real functions, no macros):

```c
void zskeyPutTreeId(u8 *a, u32 iTree);           /* 4 bytes BE */
u32  zskeyGetTreeId(const u8 *a);
void zskeyPutRowid(u8 *a, i64 rowid);            /* 8 bytes BE, sign flip */
i64  zskeyGetRowid(const u8 *a);
/* Encode nField fields of an unpacked record into an order-preserving
** byte string, appended to *pzOut (sqlite3_malloc'd, caller frees).
** Returns SQLITE_OK, SQLITE_ERROR (unsupported collation/encoding),
** or SQLITE_NOMEM. */
int  zskeyEncodeUnpacked(UnpackedRecord *pRec, int nField,
                         u8 **pzOut, int *pnOut);
/* Convenience: unpack a serialized record (as passed to BtreeInsert)
** and encode all its fields.  Uses sqlite3VdbeAllocUnpackedRecord. */
int  zskeyEncodeRecord(KeyInfo *pKeyInfo, int nRec, const void *pRec,
                       u8 **pzOut, int *pnOut);
```

- Encoding format (normative for all later tasks):
  - Per field, one type byte then payload: `0x05` NULL (or `0xFA` when `KEYINFO_ORDER_BIGNULL`), `0x07` -Inf, `0x08` negative number, `0x15` zero, `0x22` positive number, `0x23` +Inf, `0x24` text, `0x34` blob.
  - Numbers: decimal mantissa/exponent form. Normalize |x| to digits `d1 d2 ... dn` (no leading or trailing zeros) with decimal exponent `e` such that |x| = 0.d1d2...dn × 10^e. If `e` is odd, prepend a `0` digit and increment `e`. Emit `e/2 + 200` as 2 bytes BE, then base-100 digit pairs each as `2*D+1` except the final pair as `2*D`. i64 values get exact digits (decimal print); doubles via `snprintf("%.17e")` then strip. Negative numbers: type `0x08`, then every payload byte inverted (`~`). This makes `1` and `1.0` encode identically, matching SQLite's numeric comparison.
  - Text: collation transform first — BINARY: none; NOCASE: map each byte through `sqlite3UpperToLower[]`; RTRIM: drop trailing `' '` bytes — then emit with `0x00` escaped as `0x00 0x01`, terminated by `0x00 0x00`.
  - Blob: raw bytes, same escape and terminator as text.
  - `KEYINFO_ORDER_DESC`: invert (`~`) every byte the field emitted, type byte and terminator included.

- [ ] **Step 1: Write the failing property test**

`test/zskey-test.c` — a standalone binary linking `libsqlite3.a` (non-amalgamation objects export internal symbols, so internal functions link directly). Contents:

```c
/* Property test: for random record pairs A,B over supported types and
** collations, sign(memcmp(enc(A),enc(B))) must equal
** sign(sqlite3VdbeRecordCompare(nA, pA, unpack(B))). */
#include "sqliteInt.h"
#include "vdbeInt.h"
#include "zskey.h"
#include <stdio.h>

/* Minimal serializer: builds SQLite record format (header of varints:
** header-size then serial types; then body).  Supports NULL, i64,
** double, text, blob. */
typedef struct TVal TVal;
struct TVal {
  int eType;        /* 0=NULL 1=int 2=real 3=text 4=blob */
  i64 i; double r;
  const u8 *z; int n;
};
static int putVarint(u8 *p, u64 v){ return sqlite3PutVarint(p, v); }
static int serialize(TVal *aVal, int nVal, u8 *aOut){
  u8 aTypes[64]; int nTypes = 0; int nBody = 0; int i;
  u8 *pBody; int nHdr, hdrVarint;
  for(i=0; i<nVal; i++){
    TVal *v = &aVal[i];
    switch( v->eType ){
      case 0: nTypes += putVarint(aTypes+nTypes, 0); break;
      case 1: nTypes += putVarint(aTypes+nTypes, 6); nBody += 8; break;
      case 2: nTypes += putVarint(aTypes+nTypes, 7); nBody += 8; break;
      case 3: nTypes += putVarint(aTypes+nTypes, 13+2*(u64)v->n);
              nBody += v->n; break;
      case 4: nTypes += putVarint(aTypes+nTypes, 12+2*(u64)v->n);
              nBody += v->n; break;
    }
  }
  hdrVarint = sqlite3VarintLen(nTypes+1) < 2 ? 1 : sqlite3VarintLen(nTypes+hdrVarintGuess(nTypes));
  /* header size includes its own varint; sizes here are small so 1 byte */
  nHdr = 1 + nTypes;
  aOut[0] = (u8)nHdr;
  memcpy(aOut+1, aTypes, nTypes);
  pBody = aOut + nHdr;
  for(i=0; i<nVal; i++){
    TVal *v = &aVal[i];
    if( v->eType==1 ){ u64 x = (u64)v->i; int j;
      for(j=7; j>=0; j--){ pBody[j] = x & 0xff; x >>= 8; } pBody += 8;
    }else if( v->eType==2 ){ u64 x; memcpy(&x, &v->r, 8); int j;
      for(j=7; j>=0; j--){ pBody[j] = x & 0xff; x >>= 8; } pBody += 8;
    }else if( v->eType>=3 ){ memcpy(pBody, v->z, v->n); pBody += v->n; }
  }
  return (int)(pBody - aOut);
}
```

(The `hdrVarint` line above is deliberately simplified: assert `nTypes+1 < 128` and use one byte — test values stay small. Write it that way, without the bogus `hdrVarintGuess` call: `nHdr = 1 + nTypes; assert(nHdr<128);`)

Test driver: build a `KeyInfo` by hand (`sqlite3KeyInfoAlloc`-style — allocate with `sqlite3DbMallocZero(db, SZ_KEYINFO(nField))`, set `enc=SQLITE_UTF8`, `nKeyField`, `aSortFlags`, and `aColl[i] = sqlite3FindCollSeq(db, SQLITE_UTF8, zCollName, 0)` after `sqlite3_open(":memory:", &db)`). A deterministic xorshift PRNG generates ~50,000 pairs of 1–3-field records drawing from: NULL, ints (including `INT64_MIN/MAX`, ±1, 0, values >2^53), doubles (including ±0.0, ±Inf, 1.5, 1e300, 1e-300, values equal to ints), short/empty texts over `{"", "a", "A", "ab", "a b", "a\x00b" (embedded NUL), "abc   "}`, blobs `{"", "\x00", "\x00\x01", "\xff"}`; collations cycled BINARY/NOCASE/RTRIM; sort flags cycled none/DESC/BIGNULL/both. For each pair: serialize both, `zskeyEncodeRecord` both, unpack B with `sqlite3VdbeAllocUnpackedRecord`+`sqlite3VdbeRecordUnpack`, and assert sign agreement of `memcmp` vs `sqlite3VdbeRecordCompare(nA, pA, pUB)`. On mismatch print both records and encodings in hex, exit 1. Also assert: `enc(1) == enc(1.0)` byte-identical; `zskeyPutRowid`/`GetRowid` round-trip and order for `{INT64_MIN,-2,-1,0,1,2,INT64_MAX}`.

Makefile target:

```make
zskey-test$(T.exe):	$(TOP)/test/zskey-test.c $(libsqlite3.LIB)
	$(T.link) -o $@ -I. -I$(TOP)/src -I$(TOP)/ext/zeroskip \
		$(TOP)/test/zskey-test.c $(libsqlite3.LIB) $(LDFLAGS.libsqlite3)
```

- [ ] **Step 2: Run to verify it fails**

Run: `make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' zskey-test && ./zskey-test`

Expected: link failure (`zskeyEncodeRecord` undefined).

- [ ] **Step 3: Implement the codec**

`src/zskey.c`: implement per the format spec in Interfaces. Core shape:

```c
#ifdef SQLITE_ZEROSKIP
#include "sqliteInt.h"
#include "vdbeInt.h"
#include "zskey.h"

typedef struct ZsKeyOut { u8 *a; int n; int nAlloc; int rc; } ZsKeyOut;
static void outByte(ZsKeyOut *o, u8 c){ /* grow via sqlite3Realloc, append */ }
static void outBytes(ZsKeyOut *o, const u8 *a, int n){ /* ditto */ }

/* Append the number encoding for either an exact i64 or a double.
** Emits only the payload AFTER the type byte; caller chose 0x08/0x22
** and handles negative-inversion afterward via markers. */
static void encodeMagnitude(ZsKeyOut *o, char *zDigits, int e){
  int i, n = (int)strlen(zDigits);
  if( e & 1 ){ /* prepend implicit 0 digit */ ... e++; }
  outByte(o, (u8)(((e/2)+200)>>8)); outByte(o, (u8)((e/2)+200));
  for(i=0; i<n; i+=2){
    int D = (zDigits[i]-'0')*10 + (i+1<n ? zDigits[i+1]-'0' : 0);
    outByte(o, (u8)(i+2<n ? 2*D+1 : 2*D));
  }
}
```

Digits for i64: `sqlite3_snprintf` `%lld` of the absolute value (careful with `INT64_MIN`: negate as u64), strip trailing zeros, `e` = number of digits before stripping. Digits for double: `%.17e`, parse `d.dddddddddddddddde±XX`, digits = mantissa digits with trailing zeros stripped, `e` = exponent+1. DESC/negative inversion: record the output offset where the field (or magnitude payload) starts, and invert `o->a[start..n)` after emitting. Field dispatch reads `Mem` flags: `MEM_Null`, `MEM_Int` (`u.i`), `MEM_Real` (`u.r`), `MEM_IntReal` (treat as real per `sqlite3VdbeRecordCompare`... check `MEM_IntReal` handling in `vdbeaux.c` and match it), `MEM_Str` (`z`,`n` — apply collation transform), `MEM_Blob`. Collation check: `pColl==0 || strcmp(pColl->zName,"BINARY")==0` → none, `"NOCASE"` → fold, `"RTRIM"` → trim, else set `o->rc = SQLITE_ERROR`. `zskeyEncodeRecord` = alloc unpacked (`sqlite3VdbeAllocUnpackedRecord(pKeyInfo)`), `sqlite3VdbeRecordUnpack(pKeyInfo, nRec, pRec, p)`, encode `p->nField` fields, `sqlite3DbFree` the record.

- [ ] **Step 4: Run the property test until green**

Run: `make ... zskey-test && ./zskey-test`
Expected: `PASS (50000 pairs)` (make the test print that). Iterate on encoder bugs here — this is where the design earns its keep; do not weaken the test to pass.

- [ ] **Step 5: Verify stock build untouched, commit**

```bash
make sqlite3 >/dev/null && git add src/zskey.c src/zskey.h test/zskey-test.c main.mk && \
git commit -m "Add order-preserving key codec with record-compare property test"
```

---

### Task 4: Engine core — open/close, transactions, meta, tree creation

**Files:**
- Modify: `src/btree_zs.c`
- Create: `test/zsbtree-test.c`; Modify: `main.mk` (`zsbtree-test` target, same pattern as zskey-test)

**Interfaces:**
- Consumes: `zskeyPutTreeId`, `zskeyPutRowid` from Task 3; `zsbtErr` from Task 2.
- Produces (statics in btree_zs.c used by all later tasks):

```c
static int zsbtEnsureOpen(Btree *p);              /* lazy zs_db_open */
static int zsbtBegin(Btree *p, int wrflag);       /* body of BeginTrans */
/* ALL writes funnel here: undo hook (Task 8) + writeEpoch bump. */
static int zsbtWrite(Btree *p, const u8 *aKey, size_t nKey,
                     const char *pVal, size_t nVal);
static int zsbtMetaKey(u8 *a6, int idx);          /* builds [0,0,0,0,'M',idx], returns 6 */
```

Semantics to implement (replacing Task 2 stubs):

- `sqlite3BtreeOpen`: allocate `Btree`; resolve `zDir`: NULL or `":memory:"` filename → `isEphemeral=1`, `zDir = mkdtemp` of `<sqlite3_temp_directory or "/tmp">/zs-ephem-XXXXXX`; else copy `zFilename`. Do NOT open zeroskip yet (sqlite opens btrees before deciding to use them); `zsbtEnsureOpen` does `zs_db_open` with `ZS_CREATE` (unless `vfsFlags & SQLITE_OPEN_READONLY` → `ZS_SHARED`) and `ZS_NONBLOCKING`, called from `zsbtBegin`. Open the vestigial pager immediately, mirroring btree.c:2712's call: `sqlite3PagerOpen(pVfs, &p->pFakePager, 0, 0, PAGER_MEMORY|PAGER_OMIT_JOURNAL, vfsFlags|SQLITE_OPEN_MEMORY, zsbtPageReinitNoop)` — adjust arguments to the actual signature in `pager.h` (EXTRA size can be 0; if the pager insists on nonzero, pass `8`).
- `sqlite3BtreeClose`: close cursors' zs handles, `zs_txn_abort` if open, `zs_db_close`, `sqlite3PagerClose` the fake pager, free schema via `xFreeSchema`, recursive-delete `zDir` when `isEphemeral` (opendir/unlink loop then rmdir — zeroskip dirs are flat), free struct.
- `zsbtBegin(p, wrflag)`: state machine on `p->eTxn`. NONE→READ: `zs_db_begin_txn(shared=1)`. NONE→WRITE: `zs_db_begin_txn(shared=0)`. READ→WRITE upgrade: abort the read txn, begin write (cursor preservation arrives in Task 9; until then assert no open cursors hold positions across the upgrade — acceptable because tests until then don't do it). Map `ZS_LOCKED→SQLITE_BUSY`. `sqlite3BtreeBeginTrans(p,wrflag,pSchemaVersion)`: call `zsbtBegin`, then if `pSchemaVersion` read meta slot `BTREE_SCHEMA_VERSION` into it. If `p->db->nSavepoint>0 && wrflag`, call the savepoint-open hook (no-op until Task 8).
- `CommitPhaseOne`: `SQLITE_OK`. `CommitPhaseTwo`/`Commit`: `zs_txn_commit`, `eTxn=TRANS_NONE`, `iDataVersion++`, then `if( zs_db_should_repack(p->pZs) ) zs_db_repack(p->pZs);` (ignore repack errors — advisory).
- `Rollback(p, tripCode, writeOnly)`: `zs_txn_abort`; trip cursors (Task 2's TripAllCursors stub returns OK; real tripping in Task 9); `eTxn=TRANS_NONE`.
- `GetMeta(p, idx, *pValue)`: `BTREE_DATA_VERSION` → `p->iDataVersion`. Else read 6-byte meta key via `zs_txn_fetch` (open a temporary shared txn if `p->pTxn==0`, closing it after); value is 4-byte BE; absent → 0.
- `UpdateMeta(p, idx, value)`: requires write txn; 4-byte BE via `zsbtWrite`.
- `CreateTable(p, *piTable, flags)`: counter key `[0,0,0,0,'T']`; read (absent → 1, meaning "next id is 2"; id 1 is implicitly sqlite_schema), increment, store via `zsbtWrite`, `*piTable = new id`.
- `NewDb`: `SQLITE_OK` (creation is implicit).
- `zsbtWrite`: for now `zs_txn_store(p->pTxn, ...)` + `p->writeEpoch++`. (Undo hook lands in Task 8.)
- `sqlite3BtreeTxnState`: map `p->eTxn` to `SQLITE_TXN_NONE/READ/WRITE`.

- [ ] **Step 1: Write the failing C harness test**

`test/zsbtree-test.c` (links libsqlite3.a, calls internals):

```c
#include "sqliteInt.h"
#include "btree.h"
#include <stdio.h>
#include <assert.h>

int main(void){
  sqlite3 *db;
  Btree *pBt;
  int rc; u32 v = 12345; int dummy;
  system("rm -rf /tmp/zsbt-test-db");
  assert( sqlite3_open("/tmp/zsbt-ignored", &db)==SQLITE_OK || 1 );
  /* open the btree directly, bypassing sqlite3_open's schema machinery */
  rc = sqlite3BtreeOpen(0, "/tmp/zsbt-test-db", db, &pBt, 0, 0);
  assert( rc==SQLITE_OK );
  rc = sqlite3BtreeBeginTrans(pBt, 1, 0);            assert( rc==SQLITE_OK );
  rc = sqlite3BtreeUpdateMeta(pBt, BTREE_SCHEMA_VERSION, 7); assert( rc==SQLITE_OK );
  rc = sqlite3BtreeCreateTable(pBt, (Pgno*)&dummy, BTREE_INTKEY); assert( rc==SQLITE_OK );
  assert( dummy==2 );
  rc = sqlite3BtreeCommit(pBt);                      assert( rc==SQLITE_OK );
  rc = sqlite3BtreeClose(pBt);                       assert( rc==SQLITE_OK );
  /* reopen: meta persisted, next table id continues */
  rc = sqlite3BtreeOpen(0, "/tmp/zsbt-test-db", db, &pBt, 0, 0);
  assert( rc==SQLITE_OK );
  rc = sqlite3BtreeBeginTrans(pBt, 0, 0);            assert( rc==SQLITE_OK );
  sqlite3BtreeGetMeta(pBt, BTREE_SCHEMA_VERSION, &v); assert( v==7 );
  rc = sqlite3BtreeCommit(pBt);                      assert( rc==SQLITE_OK );
  rc = sqlite3BtreeBeginTrans(pBt, 1, 0);            assert( rc==SQLITE_OK );
  rc = sqlite3BtreeCreateTable(pBt, (Pgno*)&dummy, BTREE_INTKEY); assert( rc==SQLITE_OK );
  assert( dummy==3 );
  rc = sqlite3BtreeRollback(pBt, SQLITE_OK, 0);      assert( rc==SQLITE_OK );
  sqlite3BtreeClose(pBt);
  printf("zsbtree-test PASS\n");
  return 0;
}
```

(sqlite3BtreeOpen's `db` argument must be a real connection for mutexes/limits — the `":memory:"`-ish open above provides one; if `sqlite3_open` on a zeroskip build fails at this stage, use `sqlite3_open_v2` with `SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE` and ignore the schema-read error, we only need the handle. Adjust pragmatically; the assertions that matter are the Btree ones. Note BtreeOpen with `pVfs==0` must tolerate it: use `sqlite3_vfs_find(0)` when pVfs is 0 for the fake pager.)

- [ ] **Step 2: Run to verify it fails**

Run: `make ... zsbtree-test && ./zsbtree-test`
Expected: assertion failure at the first `sqlite3BtreeBeginTrans` (stub returns SQLITE_INTERNAL).

- [ ] **Step 3: Implement per the semantics block above**

- [ ] **Step 4: Run until green; also re-run zskey-test**

Expected: `zsbtree-test PASS`, and `ls /tmp/zsbt-test-db` shows zeroskip files.

- [ ] **Step 5: Commit**

```bash
git add src/btree_zs.c test/zsbtree-test.c main.mk
git commit -m "zeroskip engine: open/close, transactions, meta, tree allocation"
```

---

### Task 5: Table cursors — forward reads and writes; first real SQL

**Files:**
- Modify: `src/btree_zs.c`
- Create: `test/zs/run-tests.sh`, `test/zs/01-basic.sql`, `test/zs/01-basic.expected`

**Interfaces:**
- Consumes: `zsbtWrite`, `zskeyPutRowid`/`GetRowid`, cursor struct from Task 2.
- Produces (statics):

```c
static int zsbtCursorSeekGE(BtCursor *pCur, const u8 *aKey, size_t nKey,
                            int skipExact);
/* Position at smallest key >= aKey within the cursor's tree.
** Returns SQLITE_OK w/ eState=VALID, or SQLITE_DONE if no such key. */
static int zsbtCursorRevalidate(BtCursor *pCur);
/* If seenEpoch != pBtree->writeEpoch or eState==REQUIRESEEK, re-seek
** to the saved/borrowed key using the overlap trick (open new zs
** cursor BEFORE closing the old one). */
static int zsbtSeekLast(BtCursor *pCur);   /* interim O(n) full scan */
static int zsbtSeekLE(BtCursor *pCur, const u8 *aKey, size_t nKey,
                      int strict);         /* interim O(n); Task 7 replaces */
static void zsbtLoadCurrent(BtCursor *pCur, const char *k, size_t nk,
                            const char *v, size_t nv);
/* Sets pKey/nKey/pVal/nVal, decodes intKey when curIntKey,
** seenEpoch = writeEpoch, eState = VALID. */
```

Implementation notes (follow exactly):

- `sqlite3BtreeCursor`: fill in fields, `zskeyPutTreeId(pCur->aPrefix, iTable)`, `curIntKey = (pKeyInfo==0)`, link into `pBtree->pCursor` list. Opening a cursor on any tree id is valid even if never written (empty tree). Ephemeral btrees (BTREE_SINGLE) open cursors on tree 1 without CreateTable — works for free.
- `zsbtCursorSeekGE`: `zs_txn_begin_cursor(p->pTxn, key, n, &pZc, ZS_CURSOR_PREFIX-limited)` — **important**: the prefix bound must be the 4-byte tree prefix, but the seek key is longer; check the vendored zeroskip.h semantics of `ZS_CURSOR_PREFIX` (it bounds by the *start key* as prefix). Since our seek key is longer than the tree prefix, don't use ZS_CURSOR_PREFIX for bounded-tree iteration; instead check `memcmp(k, pCur->aPrefix, 4)==0` after each `zs_cursor_next` and treat mismatch as end-of-tree. Use `ZS_SKIPROOT` for `skipExact`.
- `sqlite3BtreeTableMoveto(pCur, intKey, bias, *pRes)`: build 12-byte key (prefix+rowid), SeekGE. Found exact → `*pRes=0`. Found greater → `*pRes=1` (cursor sits on the later entry). SQLITE_DONE → `zsbtSeekLast`; if the tree is empty, `eState=INVALID`, `*pRes=-1`; else cursor on last entry, `*pRes=-1`.
- `sqlite3BtreeInsert(pCur, pX, flags, seekResult)`: intkey path this task. Key = prefix + `zskeyPutRowid(pX->nKey)`. Value: if `pX->nZero==0`, store `pX->pData/nData` directly; else `sqlite3_malloc(nData+nZero)`, memcpy + memset tail, store, free. Call `zsbtWrite`. Then reposition the cursor on the inserted key: `zs_txn_fetch` the key (txn-lifetime pointers, A-4) and `zsbtLoadCurrent`. Ignore `seekResult` and `BTREE_APPEND` (hints); assert `(flags & BTREE_PREFORMAT)==0` (TransferRow, the only producer, is implemented without it — see below).
- `sqlite3BtreeDelete(pCur, flags)`: `zsbtWrite(key, 0-value-as-delete)` — i.e. `zs_txn_store(..., NULL, 0, 0)` routed through `zsbtWrite` with `pVal==NULL`. Save the deleted key into `aSavedKey` first, set `eState=REQUIRESEEK` (revalidate's SeekGE with `skipExact` lands on the successor, which is exactly SQLite's post-delete `BTREE_SAVEPOSITION`-with-Next contract; for the non-SAVEPOSITION case the position is simply unused).
- `sqlite3BtreeFirst`: SeekGE on the bare 4-byte prefix; `*pRes = (eState==VALID) ? 0 : 1`.
- `sqlite3BtreeNext(pCur, flags)`: `zsbtCursorRevalidate` (which re-seeks with skipExact when the position was consumed); then `zs_cursor_next`; prefix check; `SQLITE_DONE` at tree end (matching stock's return contract — check how vdbe.c interprets it: `SQLITE_DONE` means no more rows).
- `sqlite3BtreeLast`/`Previous`: `zsbtSeekLast`/`zsbtSeekLE` interim versions: full forward scan of the tree remembering the last (or last-below-target) key/value; position via fresh `zs_txn_fetch` of the remembered key. O(n) but correct.
- `sqlite3BtreePayloadSize` → `(u32)pCur->nVal`; `sqlite3BtreePayload(pCur, offset, amt, pBuf)` → bounds-check then `memcpy` from `pCur->pVal+offset`; `sqlite3BtreePayloadFetch(pCur, pAmt)` → `*pAmt = nVal; return pVal;` (borrowed, A-4). `sqlite3BtreeIntegerKey` → `pCur->intKey`. For index cursors (Task 6) PayloadSize/Payload serve the *value* too — the stored original record. 
- `sqlite3BtreeIsEmpty`: `*pRes` = 1 if First finds nothing. `sqlite3BtreeCount`: walk with a counter. `sqlite3BtreeClearTable`: loop { SeekGE(bare prefix); if done break; copy the found key to a local buffer (stack if ≤512 bytes else sqlite3_malloc); `zsbtWrite(key, NULL)` to delete; } counting into `pnChange` if non-NULL. Deliberately NOT cursor write-through (`zs_cursor_delete`): that would bypass `zsbtWrite` and thus the Task 8 undo hook. O(n) re-seeks are acceptable here.
- `sqlite3BtreeClearTableOfCursor`: ClearTable on the cursor's tree. `sqlite3BtreeDropTable`: ClearTable + `*piMoved=0`.
- `sqlite3BtreeCloseCursor`: `zs_cursor_fini`, free `aSavedKey`, unlink from list.
- `sqlite3BtreeTransferRow(pDest, pSrc, iKey)`: read pSrc's payload (borrowed), build a `BtreePayload` with `nKey=iKey`, `pData=pSrc->pVal`, call our own Insert on pDest. (This is the OP_RowData transfer path used by VACUUM.)
- `sqlite3BtreeCursorRestore(pCur, *pDifferentRow)`: `zsbtCursorRevalidate`; `*pDifferentRow` = 1 if the re-seek didn't land on the exact saved key.

- [ ] **Step 1: Write the SQL test harness and first battery**

`test/zs/run-tests.sh`:

```bash
#!/bin/bash
# Runs each test/zs/NN-*.sql against a fresh zeroskip db with ../../sqlite3zs,
# diffs against NN-*.expected.  Usage: run-tests.sh [builddir]
set -u
BUILD="${1:-.}"; SHELL_BIN="$BUILD/sqlite3zs"
SRC="$(cd "$(dirname "$0")" && pwd)"
fail=0
for t in "$SRC"/[0-9][0-9]-*.sql; do
  base="${t%.sql}"; name="$(basename "$base")"
  db="$(mktemp -d "${TMPDIR:-/tmp}/zstest-XXXXXX")/db"
  got="$("$SHELL_BIN" -batch "$db" < "$t" 2>&1)"
  want="$(cat "$base.expected")"
  if [ "$got" = "$want" ]; then echo "ok   $name"
  else echo "FAIL $name"; diff <(echo "$want") <(echo "$got") | head -20; fail=1
  fi
  rm -rf "$(dirname "$db")"
done
exit $fail
```

`test/zs/01-basic.sql`:

```sql
CREATE TABLE t1(a INTEGER PRIMARY KEY, b TEXT);
INSERT INTO t1 VALUES(1,'one'),(2,'two'),(3,'three');
SELECT a, b FROM t1;
SELECT b FROM t1 WHERE a=2;
UPDATE t1 SET b='TWO' WHERE a=2;
SELECT b FROM t1 WHERE a=2;
DELETE FROM t1 WHERE a=1;
SELECT count(*), min(a), max(a) FROM t1;
INSERT INTO t1(b) VALUES('four');
SELECT a, b FROM t1 WHERE a>3;
BEGIN;
INSERT INTO t1 VALUES(10,'ten');
ROLLBACK;
SELECT count(*) FROM t1;
BEGIN;
INSERT INTO t1 VALUES(11,'eleven');
COMMIT;
SELECT count(*) FROM t1;
CREATE TABLE t2(x);
INSERT INTO t2 SELECT b FROM t1;
SELECT count(*) FROM t2;
DROP TABLE t2;
SELECT name FROM sqlite_schema ORDER BY name;
```

Write `01-basic.expected` by running the same SQL through the **stock** `./sqlite3` shell against a scratch file db and capturing output — the engines must agree:

```bash
./sqlite3 -batch /tmp/ref.db < test/zs/01-basic.sql > test/zs/01-basic.expected
```

(Do this generation step for every battery in later tasks too; stock output is always the ground truth. Note `max(a)` and the auto-rowid `INSERT INTO t1(b)` exercise the interim `zsbtSeekLast`, and `BEGIN;...` after reads exercises the read→write upgrade — statement-scoped, no cursors held over it.)

- [ ] **Step 2: Run to verify it fails**

Run: `chmod +x test/zs/run-tests.sh && ./test/zs/run-tests.sh .`
Expected: `FAIL 01-basic` (cursor stubs return SQLITE_INTERNAL).

- [ ] **Step 3: Implement per the notes above**

- [ ] **Step 4: Run until green**

Run: `./test/zs/run-tests.sh .` → `ok 01-basic`; also `./zsbtree-test` and `./zskey-test` still pass.

- [ ] **Step 5: Commit**

```bash
git add src/btree_zs.c test/zs
git commit -m "zeroskip engine: table cursors, forward iteration, SQL end-to-end"
```

---

### Task 6: Index trees and WITHOUT ROWID

**Files:**
- Modify: `src/btree_zs.c`
- Create: `test/zs/02-indexes.sql` + `.expected` (stock-generated, as in Task 5)

**Interfaces:**
- Consumes: `zskeyEncodeUnpacked`, `zskeyEncodeRecord` (Task 3), `zsbtCursorSeekGE`/`zsbtSeekLE` (Task 5).

Implementation notes:

- `sqlite3BtreeInsert` index path (`pCur->curIntKey==0`): key = prefix + `zskeyEncodeRecord(pCur->pKeyInfo, (int)pX->nKey, pX->pKey, ...)`; value = the original `pX->pKey/nKey` bytes. An unsupported collation surfaces here as SQLITE_ERROR — set a message with `sqlite3ErrorWithMsg(pCur->pBtree->db, SQLITE_ERROR, "zeroskip: collation %s is not supported in indexes", ...)` from the codec's failure.
- `sqlite3BtreeIndexMoveto(pCur, pUnKey, *pRes)`: encoded = prefix + `zskeyEncodeUnpacked(pUnKey, pUnKey->nField, ...)`. SeekGE. Result classification: found key `k` of length `nk`; sought bytes `s` of length `ns`. If `nk>=ns && memcmp(k,s,ns)==0` → prefix-equal → `*pRes = pUnKey->default_rc` (mirrors `sqlite3VdbeRecordCompare` returning `default_rc` on all-fields-equal). Else `*pRes = +1` (found is greater — SeekGE guarantees ≥). SQLITE_DONE → `zsbtSeekLE(s, ns, strict=0)` for the "cursor on a smaller entry, res=-1" contract; empty tree → INVALID, `*pRes=-1`.
- For index cursors, `PayloadSize/Payload/PayloadFetch` already serve `pVal` = original record (Task 5 code). `sqlite3BtreeIntegerKey` is never called on index cursors (assert).
- Nothing else changes: index trees are just BLOBKEY trees and DDL flows through CreateTable + schema rows, which already work.

- [ ] **Step 1: Write the battery + stock-generated expected file**

`test/zs/02-indexes.sql`:

```sql
CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT, c REAL);
INSERT INTO t VALUES(1,'delta',4.0),(2,'alpha',1.5),(3,'Charlie',3.0),(4,'bravo',NULL),(5,'alpha',2.5);
CREATE INDEX tb ON t(b);
CREATE INDEX tbc ON t(b COLLATE NOCASE, c);
SELECT a FROM t WHERE b='alpha' ORDER BY a;
SELECT a FROM t WHERE b='charlie';
SELECT a FROM t WHERE b='charlie' COLLATE NOCASE;
SELECT b FROM t WHERE b BETWEEN 'a' AND 'c' ORDER BY b;
EXPLAIN QUERY PLAN SELECT a FROM t WHERE b='alpha';
CREATE UNIQUE INDEX tu ON t(c);
INSERT INTO t VALUES(6,'echo',3.0);
SELECT 'constraint ' || (SELECT count(*) FROM t);
CREATE TABLE wr(k TEXT, v INT, PRIMARY KEY(k, v)) WITHOUT ROWID;
INSERT INTO wr VALUES('x',2),('x',1),('y',9);
SELECT k, v FROM wr ORDER BY k, v;
SELECT v FROM wr WHERE k='x' ORDER BY v;
UPDATE wr SET v=v+10 WHERE k='y';
SELECT k, v FROM wr ORDER BY k, v;
DELETE FROM wr WHERE k='x' AND v=1;
SELECT count(*) FROM wr;
CREATE INDEX wrv ON wr(v DESC);
SELECT v FROM wr WHERE v>0 ORDER BY v;
```

Note the UNIQUE-violation INSERT: the shell prints the constraint error; stock-generated `.expected` captures the exact message. Generate expected with stock `./sqlite3` exactly as in Task 5.

Also `test/zs/03-collation-reject.sql` + expected written by hand (this one *differs* from stock deliberately):

```sql
CREATE TABLE c1(x TEXT);
SELECT icu_load_extension IS NULL FROM (SELECT NULL AS icu_load_extension);
```

Replace that placeholder-ish check with a real one: register nothing — use a built-in-but-unsupported collation instead. SQLite has no built-in fourth collation, so create one via the shell's `.testctrl`? It cannot. **Drop this file**: custom-collation rejection can't be exercised from the plain shell; instead add to `test/zsbtree-test.c` a check: after `sqlite3_create_collation(db, "weird", SQLITE_UTF8, 0, someCmp)` and `CREATE TABLE ct(x); CREATE INDEX cti ON ct(x COLLATE weird);` via `sqlite3_exec`, the subsequent `INSERT INTO ct VALUES('a')` returns SQLITE_ERROR with "not supported" in the message. (CREATE INDEX on the empty table may itself succeed — the error is allowed to surface on first insert; assert on the insert.)

- [ ] **Step 2: Run to verify failures** (02 battery fails at CREATE INDEX; zsbtree-test collation check fails)

- [ ] **Step 3: Implement per the notes**

- [ ] **Step 4: Run until green** — `run-tests.sh` all ok, `zsbtree-test` PASS, `zskey-test` PASS.

Note: `ORDER BY b` may run forward over the index (no DESC needed) but the planner is free to scan backward for some queries; the interim O(n) `Previous` keeps that correct, just slow. Correctness is what these tests assert.

- [ ] **Step 5: Commit** — `git add -A src test && git commit -m "zeroskip engine: index trees, WITHOUT ROWID, collation guard"`

---

### Task 7: Real reverse iteration

**Files:**
- Modify: `src/btree_zs.c`; possibly re-vendor `ext/zeroskip/*` (repeat Task 1 steps 1–2 if `VENDOR` says `reverse-api: MISSING`)
- Create: `test/zs/04-desc.sql` + stock-generated `.expected`

**Interfaces:**
- Consumes: the reverse API recorded in `ext/zeroskip/VENDOR` (expected spelling per the handoff: `ZS_FETCHPREV` fetch flag, `ZS_REVERSE` cursor flag; **verify against the vendored zeroskip.h and adapt** — semantics were agreed, spelling may differ).
- Produces: `zsbtSeekLast(pCur)`/`zsbtSeekLE(pCur, aKey, nKey, strict)` rewritten over reverse cursors (same signatures as Task 5); reverse cursor mode on BtCursor: add `u8 isReverse` to the struct.

Implementation notes:

- `zsbtSeekLast(pCur)`: reverse cursor with the 4-byte prefix as start (empty-suffix = last key under prefix, per handoff item "ZS_CURSOR_PREFIX composes"). Here ZS_CURSOR_PREFIX *is* usable (start key == the prefix itself): the reverse cursor stops leaving the prefix on its own.
- `zsbtSeekLE`: reverse cursor seeded with the full seek key, `ZS_SKIPROOT` when strict; first `zs_cursor_next` (reverse) yields the predecessor-or-equal; then verify the 4-byte prefix.
- `sqlite3BtreePrevious`: if the cursor's zs cursor is forward (or stale), re-seek via `zsbtSeekLE(current key, strict=1)` using the overlap trick; thereafter each Previous is one reverse `zs_cursor_next`. Symmetrically, `Next` on a reverse-mode cursor re-seeks GE-strict. Set/clear `isReverse` at each re-seek.
- `TableMoveto`/`IndexMoveto` EOF paths and `IndexMoveto`'s SQLITE_DONE branch now use the real `zsbtSeekLE`.
- Delete the interim O(n) scan bodies entirely.

- [ ] **Step 1: Re-vendor if needed; verify API**

Run: `grep -n "ZS_REVERSE\|ZS_FETCHPREV" ext/zeroskip/zeroskip.h`
Expected: both present (or the equivalents noted in VENDOR). Read their doc comments fully before coding; the handoff asked for specific `ZS_SKIPROOT`/`ZS_CURSOR_PREFIX` compositions — confirm each, and if any semantic differs from the handoff, adapt our call sites (never patch vendored code).

- [ ] **Step 2: Write the battery + stock-generated expected**

`test/zs/04-desc.sql`:

```sql
CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);
INSERT INTO t VALUES(1,'a'),(5,'e'),(3,'c'),(4,'d'),(2,'b');
SELECT a FROM t ORDER BY a DESC;
SELECT b FROM t ORDER BY a DESC LIMIT 2;
SELECT max(a) FROM t;
SELECT a FROM t WHERE a<4 ORDER BY a DESC;
SELECT a FROM t WHERE a<=4 ORDER BY a DESC LIMIT 1;
CREATE INDEX tb ON t(b);
SELECT b FROM t ORDER BY b DESC;
SELECT b FROM t WHERE b<'d' ORDER BY b DESC;
DELETE FROM t WHERE a=5;
SELECT max(a) FROM t;
INSERT INTO t(b) VALUES('z');
SELECT a,b FROM t ORDER BY a DESC LIMIT 1;
CREATE TABLE big(x INTEGER PRIMARY KEY);
INSERT INTO big SELECT value FROM generate_series(1,10000);
SELECT count(*), max(x) FROM big;
SELECT x FROM big ORDER BY x DESC LIMIT 3;
```

(If `generate_series` isn't in the shell build, use a recursive CTE: `WITH RECURSIVE s(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM s WHERE x<10000) INSERT INTO big SELECT x FROM s;`)

- [ ] **Step 3: Run to confirm current state** — battery passes *slowly* on the O(n) fallback or fails if fallback was incomplete; either way note the `time ./test/zs/run-tests.sh` baseline.

- [ ] **Step 4: Implement; run until green and fast**

Expected: all batteries ok; the 10k-row insert section completes in well under a second (each INSERT's max-rowid probe is now a single reverse seek).

- [ ] **Step 5: Commit** — `git commit -am "zeroskip engine: native reverse iteration for Last/Prev/LE seeks"`

---

### Task 8: Savepoints and statement rollback

**Files:**
- Modify: `src/btree_zs.c`
- Create: `test/zs/05-savepoints.sql` + stock-generated `.expected`

**Interfaces:**
- Consumes: `zsbtWrite` (every mutation already funnels through it: Insert, Delete, ClearTable, UpdateMeta, CreateTable's counter).
- Produces:

```c
typedef struct ZsUndoEntry ZsUndoEntry;
struct ZsUndoEntry {
  u8 *aKey;            /* sqlite3_malloc'd copy, WITH tree prefix */
  int nKey;
  const char *pOld;    /* borrowed txn-lifetime value ptr, or 0 = absent */
  size_t nOld;
};
/* replaces Task 2's placeholder ZsUndo */
struct ZsUndo {
  ZsUndoEntry *a; int n; int nAlloc;
  int *aMark; int nMark; int nMarkAlloc;
};
static int  zsbtSavepointTo(Btree *p, int nTarget);  /* push marks to nTarget */
static void zsbtUndoReset(ZsUndo*);                  /* free keys, zero counts */
```

Semantics:

- `zsbtWrite` gains a prologue: if `p->undo.nMark > 0`, `zs_txn_fetch` the key first (exact); push `{keycopy, pOld-or-0, nOld}`. Borrowed `pOld` is A-4-safe: valid until txn end, and the undo log never survives the txn.
- `zsbtSavepointTo(p, n)`: `while( nMark < n ) aMark[nMark++] = p->undo.n;`
- Call sites (mirroring stock btree's lazy `sqlite3PagerOpenSavepoint` usage): in `sqlite3BtreeBeginTrans` after a write txn is open, `if( p->db->nSavepoint ) zsbtSavepointTo(p, p->db->nSavepoint);` and `sqlite3BtreeBeginStmt(p, iStatement)` → `zsbtSavepointTo(p, iStatement)` (vdbe.c:4305 passes `p->iStatement`).
- `sqlite3BtreeSavepoint(p, op, iSavepoint)`: guard `p && p->eTxn==TRANS_WRITE`. First read the authoritative semantics in the comment above `sqlite3PagerSavepoint` in `src/pager.c` and confirm the following matches (fix here if not): iSavepoint of -1 with SAVEPOINT_ROLLBACK = rollback the whole txn (stock forwards that case to BtreeRollback — check vdbeaux.c:3241 context). Otherwise:
  - `SAVEPOINT_RELEASE`: `nMark = iSavepoint;` — drop the marks only, keep all undo entries: an outer savepoint's ROLLBACK still needs the released savepoint's history (its own watermark is smaller). Entries are freed at txn end.
  - `SAVEPOINT_ROLLBACK`: replay `undo.a[undo.n-1 .. aMark[iSavepoint]]` in reverse: absent → `zs_txn_store(key, NULL, 0, 0)`, else `zs_txn_store(key, pOld, nOld, 0)` — direct zs calls, NOT `zsbtWrite` (no undo-of-undo). Free replayed keys, `undo.n = aMark[iSavepoint]`, `nMark = iSavepoint+1`, `writeEpoch++`, trip open cursors to REQUIRESEEK (they hold borrowed pointers into records that are still valid memory per A-4, but logically stale — copy each VALID cursor's key to `aSavedKey` before bumping, exactly the trip helper Task 9 formalizes; write the simple version here and let Task 9 refactor).
- Commit/Rollback of the whole txn: `zsbtUndoReset`.

- [ ] **Step 1: Write the battery + stock-generated expected**

`test/zs/05-savepoints.sql`:

```sql
CREATE TABLE t(a INTEGER PRIMARY KEY, b);
INSERT INTO t VALUES(1,'one'),(2,'two');
BEGIN;
UPDATE t SET b='ONE' WHERE a=1;
SAVEPOINT s1;
DELETE FROM t WHERE a=2;
INSERT INTO t VALUES(3,'three');
SAVEPOINT s2;
UPDATE t SET b='THREE' WHERE a=3;
ROLLBACK TO s2;
SELECT a,b FROM t ORDER BY a;
ROLLBACK TO s1;
SELECT a,b FROM t ORDER BY a;
RELEASE s1;
COMMIT;
SELECT a,b FROM t ORDER BY a;
CREATE TABLE u(x UNIQUE);
INSERT INTO u VALUES(1),(2),(3);
UPDATE OR FAIL u SET x=x+1;
SELECT x FROM u ORDER BY x;
SAVEPOINT o;
INSERT INTO t VALUES(9,'nine');
ROLLBACK TO o;
RELEASE o;
SELECT count(*) FROM t;
```

The `UPDATE OR FAIL` hits a UNIQUE collision mid-statement, exercising the statement journal (partial statement must persist per FAIL semantics — stock output is ground truth). Add a second variant with plain `UPDATE` (ABORT semantics: statement fully rolled back).

- [ ] **Step 2: Run to verify failure** — savepoint statements error or produce wrong rows (BtreeSavepoint is a Task 2 stub).

- [ ] **Step 3: Implement per semantics above**

- [ ] **Step 4: Run all batteries + C harnesses until green**

- [ ] **Step 5: Commit** — `git commit -am "zeroskip engine: savepoints and statement rollback via undo log"`

---

### Task 9: Cursor resilience — upgrade preservation, trip/restore

**Files:**
- Modify: `src/btree_zs.c`
- Create: `test/zs/06-cursors.sql` + stock-generated `.expected`

**Interfaces:**
- Produces: `static int zsbtSaveAllCursors(Btree *p)` — for every cursor with `eState==ZS_CUR_VALID`: copy `pKey/nKey` into `aSavedKey/nSavedKey` (sqlite3_malloc), `zs_cursor_fini` its zs cursor, `eState=ZS_CUR_REQUIRESEEK`. Used by: read→write upgrade (replaces Task 4's assert), Task 8's post-ROLLBACK-TO trip (refactor it to call this), and `sqlite3BtreeTripAllCursors`.

Semantics:

- Read→write upgrade in `zsbtBegin`: `zsbtSaveAllCursors(p)` → abort read txn → begin write txn → cursors lazily re-seek via `zsbtCursorRevalidate` (which already handles REQUIRESEEK from `aSavedKey`).
- `sqlite3BtreeTripAllCursors(p, errCode, writeOnly)`: if `errCode==SQLITE_OK`... stock semantics: mark cursors so the next op returns errCode (or just saves position when errCode is 0). Implement: `zsbtSaveAllCursors`, and when `errCode!=SQLITE_OK` set `eState=ZS_CUR_FAULT, skipNext=errCode` on each (skip read-only cursors when `writeOnly` and the cursor is read-only... stock: `writeOnly` trips only write cursors — mirror: `if( writeOnly && !pCur->wrFlag ) continue;` for the FAULT part, but still save positions).
- `zsbtCursorRevalidate` handles all three stale states: FAULT → return `skipNext`; REQUIRESEEK → SeekGE from `aSavedKey` (overlap unnecessary, no live borrow), set `*pDifferentRow` info for CursorRestore via exact-match check; epoch-stale VALID → overlap re-seek.
- `sqlite3BtreeCursorHasMoved` / `sqlite3BtreeCursorRestore`: already wired (Tasks 2/5); confirm Restore reports `*pDifferentRow` correctly for a row deleted out from under the cursor.

- [ ] **Step 1: Write the battery + stock-generated expected**

`test/zs/06-cursors.sql`:

```sql
CREATE TABLE t(a INTEGER PRIMARY KEY, b);
INSERT INTO t VALUES(1,1),(2,2),(3,3),(4,4);
INSERT INTO t SELECT a+4, b FROM t;
SELECT count(*) FROM t;
BEGIN;
SELECT count(*) FROM t;
INSERT INTO t VALUES(100,100);
SELECT count(*) FROM t;
COMMIT;
UPDATE t SET b=b+1 WHERE a IN (SELECT a FROM t WHERE b%2=0);
SELECT sum(b) FROM t;
DELETE FROM t WHERE b IN (SELECT b FROM t WHERE a>6);
SELECT count(*) FROM t;
```

`INSERT INTO t SELECT ... FROM t` is the canonical write-under-open-cursor case; `BEGIN; SELECT; INSERT` is the upgrade-with-history case (upgrade happens at the INSERT with the read txn already snapshot by the SELECT).

- [ ] **Step 2: Run to verify current behavior** — likely assertion failure or wrong count on the self-insert (Task 4's upgrade assert, or missing epoch handling).

- [ ] **Step 3: Implement; Step 4: all green; Step 5: Commit**

```bash
git commit -am "zeroskip engine: cursor preservation across upgrade, trip and restore"
```

---

### Task 10: Backup API

**Files:**
- Modify: `src/backup_zs.c` (replace stubs), `src/btree_zs.c` (expose two tiny helpers)
- Create: `test/zs/07-backup.sql` + `.expected` (hand-written — uses `.backup`), `test/zs/backup-concurrent.sh`

**Interfaces:**
- Consumes from btree_zs.c (make these non-static, prefixed `sqlite3ZsBtree*`, declared in a small `src/btree_zs.h`): `sqlite3ZsBtreeDb(Btree*) -> struct zs_db*` (ensures open), `sqlite3ZsBtreeEnterSnapshot/Leave` — or simpler: `struct zs_db *sqlite3ZsBtreeHandle(Btree *p)` plus using public zeroskip calls directly from backup_zs.c. Take the simple route: one accessor returning `p->pZs` (after `zsbtEnsureOpen`), plus `int sqlite3ZsBtreeInvalidate(Btree*)` that bumps `writeEpoch` and `iDataVersion` on the destination after the copy.
- Produces: working `sqlite3_backup_*` family + `sqlite3BtreeCopyFile`.

Implementation:

```c
struct sqlite3_backup {
  sqlite3 *pDestDb; Btree *pDest;
  sqlite3 *pSrcDb;  Btree *pSrc;
  struct zs_txn *pSrcTxn;   /* shared snapshot, opened on first step */
  struct zs_txn *pDestTxn;  /* write txn */
  struct zs_cursor *pCur;   /* walk of the entire source keyspace */
  int rc;                   /* sticky */
  int nRemaining, nPagecount;
  int isAttached;           /* fake: we never attach, kept for API parity */
};
```

- `init`: resolve Btrees via the same lookup stock backup.c uses (`sqlite3DbNameToBtree` — grep it in `src/backup.c` and reuse); error if either is in a write txn... stock allows source read use; keep it simple: fail with SQLITE_BUSY if `pDest` has any open txn. Set `pDest->inBackup=1`.
- first `step`: source: `zs_db_begin_txn(shared=1)`; count records with a full-cursor pre-scan (`nPagecount = nRemaining = count`); destination: write txn; clear destination: loop-fetch-first-and-delete over the whole keyspace (all trees AND meta — a backup replaces everything).
- `step(nPage)`: copy `nPage<0 ? all : max(nPage,1)*16` records: `zs_cursor_next` on source, `zs_txn_store` on dest (borrowed pointers straight through). Decrement `nRemaining`. Return `SQLITE_DONE` when the walk ends.
- `finish`: commit dest txn (abort if `rc` is an error), end source txn, `sqlite3ZsBtreeInvalidate(pDest)`, clear `inBackup`.
- `sqlite3BtreeCopyFile(pTo, pFrom)`: run the whole thing in one shot with a stack `sqlite3_backup` (mirroring stock's approach — see how `backup.c` does exactly this; it must work inside the write txn VACUUM holds, so instead of opening new txns reuse `pTo->pTxn`/`pFrom->pTxn` when already present; only open/close what wasn't open).

- [ ] **Step 1: Write tests**

`test/zs/07-backup.sql` + hand-written expected (shell `.backup` prints nothing on success):

```sql
CREATE TABLE t(a INTEGER PRIMARY KEY, b);
INSERT INTO t VALUES(1,'one'),(2,'two');
CREATE INDEX tb ON t(b);
.backup main /tmp/zs-backup-dest
.open /tmp/zs-backup-dest
SELECT a,b FROM t ORDER BY a;
PRAGMA integrity_check;
```

Add `rm -rf /tmp/zs-backup-dest` to the runner before each test (or better: make the runner export `ZSTMP=$(mktemp -d)` and have the SQL say `.backup main ZSTMP/dest` — shells can't expand env in dot-commands, so simplest is the fixed `/tmp/zs-backup-dest` path plus cleanup in `run-tests.sh`: add `rm -rf /tmp/zs-backup-dest` before the invocation loop body).

`test/zs/backup-concurrent.sh`: starts a writer loop (`while true; do ./sqlite3zs $DB "INSERT INTO t(b) VALUES(hex(randomblob(8)));"; done &`), runs `.backup` via a second shell invocation mid-stream, kills the writer, then `PRAGMA integrity_check` on the backup and asserts row count in the backup is self-consistent (`SELECT count(*)==max(rowid_gaps_ok)` — concretely: `PRAGMA integrity_check;` returns `ok` and `SELECT count(*)>=2` succeeds).

- [ ] **Step 2: Run to verify failure** — `.backup` reports "backup not yet supported".

- [ ] **Step 3: Implement; Step 4: green** (both scripts; plus stock-vs-zs equivalence: `.dump` the source and the backup, diff).

Also verify VACUUM now works end-to-end: add to `07-backup.sql`: `VACUUM;` then `SELECT count(*) FROM t;` (VACUUM drives BtreeCopyFile).

- [ ] **Step 5: Commit** — `git add -A src test && git commit -m "zeroskip engine: online backup API and BtreeCopyFile"`

---

### Task 11: Integrity check, crash recovery, multi-process

**Files:**
- Modify: `src/btree_zs.c` (IntegrityCheck)
- Create: `test/zs/crash-test.sh`, `test/zs/busy-test.sh`

**Interfaces:**
- Consumes: `zs_db_check_consistency`, `zskeyGetTreeId`.

Implementation of `sqlite3BtreeIntegrityCheck(db, p, aRoot, aCnt, nRoot, mxErr, pnErr, pzOut)`:

- `zs_db_check_consistency(p->pZs)` != ZS_OK → one error line "zeroskip consistency check failed (%s)".
- Walk the entire keyspace once with a cursor: every key must be ≥5 bytes (prefix+something) with a tree id that is 0 or appears in `aRoot[]` (ids not in aRoot get one "stray tree %u" error each — collect seen strays in a small array); count records per root into `aCnt[i]` via `sqlite3_result_int64`-style assignment (aCnt is `sqlite3_value*` — check how stock fills it: grep `aCnt` in `src/btree.c` and mirror the mechanism, `sqlite3ValueSetNull`/`sqlite3VdbeMemSetInt64`).
- Errors accumulate into `*pzOut` via `sqlite3_str` (mirror stock's use of `sqlite3StrAccum` in `checkAppendMsg`), capped by `mxErr` into `*pnErr`.

- [ ] **Step 1: SQL check** — append to `01-basic.sql`: `PRAGMA integrity_check;` (+regenerate expected with stock: emits `ok`). `PRAGMA quick_check;` too.

- [ ] **Step 2: crash-test.sh**

```bash
#!/bin/bash
# Writer inserts in a loop; kill -9 it mid-flight; reopen and verify.
set -eu
BUILD="${1:-.}"; DB="$(mktemp -d)/db"
"$BUILD/sqlite3zs" "$DB" 'CREATE TABLE t(a INTEGER PRIMARY KEY, b);'
"$BUILD/sqlite3zs" "$DB" 'WITH RECURSIVE s(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM s WHERE x<100000) INSERT INTO t SELECT x, hex(randomblob(64)) FROM s;' &
W=$!
sleep 0.2; kill -9 $W 2>/dev/null; wait $W 2>/dev/null || true
ic="$("$BUILD/sqlite3zs" "$DB" 'PRAGMA integrity_check; SELECT count(*)>=0 FROM t;')"
echo "$ic" | grep -qx ok || { echo "FAIL: integrity_check: $ic"; exit 1; }
echo "crash-test ok (count query also ran)"
```

Run it ~10 times in a loop (timing varies which phase dies). Expected: always `ok` — either the whole insert committed or none of it (single txn).

- [ ] **Step 3: busy-test.sh** — two shells: one holds `BEGIN IMMEDIATE; INSERT ...;` open (via `(echo 'BEGIN IMMEDIATE; INSERT INTO t VALUES(1,1);'; sleep 2; echo 'COMMIT;') | sqlite3zs $DB` in background), the second sets `.timeout 5000` and inserts — must succeed after the first commits; a third with `.timeout 1` must print `database is locked`. Also verify a plain reader (`SELECT count(*)`) succeeds instantly while the writer holds its txn (lock-free readers).

- [ ] **Step 4: All green; commit** — `git commit -am "zeroskip engine: integrity check, crash and concurrency tests"`

---

### Task 12: Benchmarks, stock regression, docs

**Files:**
- Modify: `main.mk` (speedtest1-zs target)
- Create: `doc/zeroskip-engine.md`

- [ ] **Step 1: Add benchmark target**

```make
speedtest1zs$(T.exe):	$(TOP)/test/speedtest1.c $(libsqlite3.LIB)
	$(T.link) $(ST_OPT) -o $@ -I. -I$(TOP)/src \
		$(TOP)/test/speedtest1.c $(libsqlite3.LIB) $(LDFLAGS.libsqlite3)
```

- [ ] **Step 2: Run the comparison**

```bash
make USE_AMALGAMATION=0 OPTIONS='-DSQLITE_ZEROSKIP -DSQLITE_OMIT_SHARED_CACHE' speedtest1zs
./speedtest1zs --size 5 /tmp/zs-bench-db 2>&1 | tail -3
make speedtest1 && ./speedtest1 --size 5 /tmp/stock-bench.db 2>&1 | tail -3
```

Expected: zeroskip build completes without errors (some testsets exercise DESC scans, vacuum, etc. — any hard failure is a bug to fix before proceeding; slower is fine, wrong is not). Record both timings in `doc/zeroskip-engine.md`.

- [ ] **Step 3: Stock regression check**

```bash
git stash list >/dev/null  # ensure clean tree
make sqlite3 testfixture >/dev/null && ./testfixture test/veryquick.test 2>&1 | tail -3
```

Expected: same pass/fail profile as an unmodified checkout (the guards are the only stock-visible change). If testfixture won't build from this tree for unrelated reasons, `make sqlite3 && ./sqlite3 :memory: 'select 1'` plus a diff of `git diff master -- src/btree.c src/backup.c` showing only the guard lines is the acceptable fallback.

- [ ] **Step 4: Write doc/zeroskip-engine.md**

Contents: how to build (the canonical command), what works, what doesn't (the spec's stub list verbatim), the key-encoding format (copy from Task 3 Interfaces), benchmark numbers, re-vendoring procedure (`ext/zeroskip/VENDOR`), and the backup-semantics deviation note.

- [ ] **Step 5: Final full run + commit**

```bash
./zskey-test && ./zsbtree-test && ./test/zs/run-tests.sh . && \
  ./test/zs/crash-test.sh . && ./test/zs/busy-test.sh . && \
git add -A && git commit -m "zeroskip engine: benchmarks and documentation"
```

---

## Plan self-review notes (resolved inline)

- Spec coverage: build swap (T2), vendoring (T1), key layout+codec (T3), txns/meta (T4), cursors zero-copy + forward (T5), indexes/WITHOUT ROWID/collation guard (T6), reverse (T7), savepoints/undo (T8), upgrade/trip/restore (T9), backup+CopyFile+VACUUM (T10), integrity/crash/multiprocess (T11), speedtest+docs+stock regression (T12). Repack-after-commit: T4 commit path. Ephemeral btrees: T4 open path, exercised implicitly by subquery/IN batteries (T6/T9).
- Known intentional deviations from spec text: VACUUM primarily via the stock CopyFile path (T10) with `zs_db_compact` left unwired — repack-after-commit covers space reclamation for the PoC; note it in doc/zeroskip-engine.md.
- The `serialize()` sketch in T3 contains a deliberately flagged simplification (single-byte header) — the step text says exactly what to write.
