-- Keys past the pending set's 64-byte inline bound (ZSI_PEND_KPREFIX), where
-- a comparison has to read the record because the inlined prefixes tie.  The
-- shapes that reach it: a long shared literal head, duplicate index values
-- distinguished only by the appended rowid, and the bound itself at 64/65.
-- Insertion order is deliberately not ascending, so the skiplist splices
-- rather than appends and the arena grows under a live transaction.

-- 1.  WITHOUT ROWID TEXT PK under a 58-character common directory: every
--     comparison between two of these agrees for the whole inlined prefix.
CREATE TABLE p(path TEXT PRIMARY KEY, sz INT) WITHOUT ROWID;
BEGIN;
WITH RECURSIVE c(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM c WHERE i<499)
INSERT INTO p
  SELECT '/var/spool/imap/domain/example.com/user/brong/INBOX/Lists/'
         || printf('%04d', (i*277)%500) || '/' || printf('%06d', (i*97)%500),
         i
    FROM c;
COMMIT;
SELECT count(*), min(length(path)), max(length(path)), sum(sz) FROM p;
SELECT path FROM p ORDER BY path LIMIT 2;
SELECT path FROM p ORDER BY path DESC LIMIT 2;
SELECT count(*) FROM p WHERE path > '/var/spool/imap/domain/example.com/user/brong/INBOX/Lists/0250';
SELECT sz FROM p WHERE path =
  '/var/spool/imap/domain/example.com/user/brong/INBOX/Lists/0277/000097';

-- 2.  The bound itself.  A WITHOUT ROWID TEXT PK encodes to length+7 bytes,
--     so 57 characters is exactly 64 and 58 is the first key past it.  A key
--     that is a strict prefix of another is the case F-11a's length rule has
--     to settle without reading anything.
CREATE TABLE b(k TEXT PRIMARY KEY) WITHOUT ROWID;
INSERT INTO b VALUES
  (substr('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',1,56)),
  (substr('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',1,57)),
  (substr('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',1,58)),
  (substr('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',1,59)),
  (substr('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',1,57) || 'z'),
  (substr('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',1,57) || 'a');
SELECT length(k) FROM b ORDER BY k;
SELECT count(*) FROM b WHERE k >
  substr('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',1,57);

-- 3.  An index whose values repeat: entries differ only in the rowid that
--     SQLite appends, which sits past the bound for a value this long.
CREATE TABLE m(id INTEGER PRIMARY KEY, subject TEXT);
CREATE INDEX m_subject ON m(subject);
BEGIN;
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<400)
INSERT INTO m
  SELECT (i*163)%400 + 1,
         'Re: Re: Fwd: [sqlite] notes from the standup, and what we owe '
         || (CASE i%4 WHEN 0 THEN 'the customer' ELSE 'everyone else' END)
    FROM c;
SAVEPOINT s1;
UPDATE m SET subject = subject || ' (edited)' WHERE id%3 = 0;
SELECT count(*) FROM m WHERE subject LIKE '%(edited)';
ROLLBACK TO s1;
SELECT count(*) FROM m WHERE subject LIKE '%(edited)';
DELETE FROM m WHERE id%7 = 0;
RELEASE s1;
COMMIT;
SELECT count(*), count(DISTINCT subject) FROM m;
SELECT subject, count(*) FROM m GROUP BY subject ORDER BY subject;
SELECT id FROM m WHERE subject LIKE '%the customer' ORDER BY id LIMIT 3;

-- 4.  Cursors walking long keys while the same transaction writes: the
--     scan and the stores share the arena the writes are growing.
BEGIN;
INSERT INTO p SELECT
  '/var/spool/imap/domain/example.com/user/brong/INBOX/Lists/9999/'
  || printf('%06d', sz), sz + 100000
  FROM p WHERE sz < 50;
SELECT count(*) FROM p;
SELECT count(*) FROM p WHERE path LIKE '%/9999/%';
SELECT path FROM p ORDER BY path DESC LIMIT 3;
DELETE FROM p WHERE path LIKE '%/9999/%' AND sz%2 = 0;
SELECT count(*) FROM p;
ROLLBACK;
SELECT count(*), sum(sz) FROM p;

-- 5.  Long keys and short keys interleaved in one transaction, so the
--     inlined-whole and inlined-prefix paths splice against each other.
CREATE TABLE mix(k TEXT PRIMARY KEY, v INT) WITHOUT ROWID;
BEGIN;
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<300)
INSERT INTO mix
  SELECT CASE i%3
           WHEN 0 THEN printf('%03d', (i*211)%300)
           WHEN 1 THEN 'medium-length-key-' || printf('%03d', (i*211)%300)
           ELSE '/a/very/long/shared/head/that/runs/well/past/the/inline/bound/'
                || printf('%03d', (i*211)%300)
         END, i
    FROM c;
COMMIT;
SELECT count(*), sum(v) FROM mix;
SELECT k FROM mix ORDER BY k LIMIT 3;
SELECT k FROM mix ORDER BY k DESC LIMIT 3;
SELECT count(*) FROM mix WHERE k < 'm';
PRAGMA integrity_check;
