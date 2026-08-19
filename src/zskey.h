/*
** 2026-08-11
**
** Order-preserving key encoding for the zeroskip storage engine.
**
** Every zeroskip key is [4-byte BE tree-id][payload].  For INTKEY
** (rowid) trees the payload is the rowid, 8 bytes big-endian with the
** sign bit flipped so that memcmp order equals signed order.  For
** index (BLOBKEY) trees the payload is an order-preserving encoding of
** the index record such that memcmp on encodings agrees with
** sqlite3VdbeRecordCompare on the records.
**
** Per-field format: one type byte, then a payload:
**   0x05  NULL       (0xFA when KEYINFO_ORDER_BIGNULL)
**   0x07  -Infinity
**   0x08  negative number   payload = magnitude encoding, all bytes ~inverted
**   0x15  zero
**   0x22  positive number   payload = magnitude encoding
**   0x23  +Infinity
**   0x24  text       payload = collation transform, 0x00 escaped as 0x00 0x01,
**                    terminated by 0x00 0x00
**   0x34  blob       raw bytes, same escape and terminator as text
**
** Magnitude encoding: normalize |x| to decimal digits d1..dn (no
** leading or trailing zeros) and exponent e with |x| = 0.d1..dn * 10^e;
** if e is odd prepend a zero digit and increment e; emit (e/2)+200 as
** two bytes big-endian, then base-100 digit pairs, each byte 2*D+1
** except the final pair which is 2*D.  Integers get their exact decimal
** digits, so 64-bit values beyond 2^53 order correctly, and 1 encodes
** identically to 1.0 (matching SQLite numeric comparison).
**
** KEYINFO_ORDER_DESC inverts (~) every byte the field emitted, type
** byte and terminator included.
**
** Supported collations: BINARY (no transform), NOCASE (bytes mapped
** through sqlite3UpperToLower[]), RTRIM (trailing 0x20 stripped).
** Anything else, or a non-UTF8 KeyInfo, fails with SQLITE_ERROR.
*/
#ifndef SQLITE_ZSKEY_H
#define SQLITE_ZSKEY_H

void zskeyPutTreeId(u8 *a, u32 iTree);
u32  zskeyGetTreeId(const u8 *a);
void zskeyPutRowid(u8 *a, i64 rowid);
i64  zskeyGetRowid(const u8 *a);

/* Reusable output buffer: a is sqlite3 heap memory that grows and is
** reused across encodes (caller frees a with sqlite3_free), n is the
** current length, nAlloc the capacity. */
typedef struct ZsKeyBuf ZsKeyBuf;
struct ZsKeyBuf {
  u8 *a;
  int n;
  int nAlloc;
};

/* Append raw bytes to a ZsKeyBuf (used for tree-id prefixes and seek
** sentinels).  Returns SQLITE_OK or SQLITE_NOMEM. */
int zskeyBufAppend(ZsKeyBuf *pBuf, const u8 *a, int n);

/* Encode the first nField fields of an unpacked record, APPENDING to
** pBuf.  Returns SQLITE_OK, SQLITE_ERROR (unsupported collation or
** text encoding) or SQLITE_NOMEM. */
int zskeyEncodeUnpackedBuf(UnpackedRecord *pRec, int nField,
                           ZsKeyBuf *pBuf);

/* Unpack a serialized record (as passed to sqlite3BtreeInsert for an
** index tree) and encode all of its fields, appending to pBuf.
** *ppScratch is a reusable UnpackedRecord for pKeyInfo: pass the same
** pointer across calls, initialized to 0; the caller frees it with
** sqlite3DbFree(pKeyInfo->db, *ppScratch). */
int zskeyEncodeRecordBuf(KeyInfo *pKeyInfo, int nRec, const void *pRec,
                         UnpackedRecord **ppScratch, ZsKeyBuf *pBuf);

/* One-shot wrappers around the Buf forms (used by tests). */
int zskeyEncodeUnpacked(UnpackedRecord *pRec, int nField,
                        u8 **pzOut, int *pnOut);
int zskeyEncodeRecord(KeyInfo *pKeyInfo, int nRec, const void *pRec,
                      u8 **pzOut, int *pnOut);

#endif /* SQLITE_ZSKEY_H */
