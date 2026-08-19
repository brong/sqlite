-- The savepoint undo log, where the append bound is allowed to answer for it.
--
-- zsbtWrite saves a row's current version before overwriting it.  When the
-- key is STRICTLY ABOVE the tree's exact append bound there is nothing there
-- to save, so the fetch can be skipped and the entry recorded as "was
-- absent".  Everything here is a way for that shortcut to be wrong.
--
-- The shapes that discriminate:
--
--   * a key EQUAL to the bound is present, so a `>=` comparison loses a
--     live before-image and rollback deletes a row instead of restoring it;
--   * a key written TWICE inside one savepoint is above the bound the first
--     time and equal to it the second -- but only if the funnel raised the
--     bound as it went, which is the invariant that makes this sound;
--   * a statement journal, not a savepoint, is what opens an undo mark for
--     ordinary multi-row DML, so a constraint failure part-way through an
--     INSERT ... SELECT must undo that statement's earlier rows.
--
-- Sections 10 to 15 are the other half of the same subject: the before-image
-- taken from a CURSOR already sitting on the row (zsbtUndoHint) rather than
-- fetched.  What discriminates there:
--
--   * OP_NewRowid leaves the cursor on the LARGEST key while the insert goes
--     to a new one, so a hint that trusted the cursor without comparing keys
--     saves the wrong row's value (section 11);
--   * a row written more than once must see the version the previous write
--     left, not the one the cursor loaded (section 12);
--   * an UPDATE that moves a rowid deletes the cursor's key and inserts one
--     no cursor is on (section 14).
--
-- Ground truth is the stock btree: this file is run through both engines and
-- the output must match.

CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);
INSERT INTO t VALUES(10,'ten'),(20,'twenty'),(30,'thirty');

-- 1. insert above the bound, then take it back
BEGIN;
SAVEPOINT s1;
SELECT a,b FROM t WHERE a<=999 ORDER BY a DESC LIMIT 1;   -- seeds the bound
INSERT INTO t VALUES(40,'forty');
SELECT a,b FROM t ORDER BY a;
ROLLBACK TO s1;
SELECT a,b FROM t ORDER BY a;
RELEASE s1;
COMMIT;
SELECT count(*), max(a) FROM t;

-- 2. overwrite the row that IS the bound: the before-image is live and must
--    come back.  A `>=` comparison in the shortcut deletes row 30 instead.
BEGIN;
SELECT a,b FROM t WHERE a<=999 ORDER BY a DESC LIMIT 1;   -- bound = 30
SAVEPOINT s2;
UPDATE t SET b='THIRTY' WHERE a=30;
SELECT a,b FROM t WHERE a=30;
ROLLBACK TO s2;
SELECT a,b FROM t WHERE a=30;
SELECT count(*) FROM t;
RELEASE s2;
COMMIT;
SELECT a,b FROM t ORDER BY a;

-- 3. the same key written twice inside one savepoint.  The second write is
--    at the bound, not above it, and only because the first write raised it.
BEGIN;
SELECT a,b FROM t WHERE a<=999 ORDER BY a DESC LIMIT 1;
SAVEPOINT s3;
INSERT INTO t VALUES(50,'fifty-first');
SAVEPOINT s4;
INSERT OR REPLACE INTO t VALUES(50,'fifty-second');
SELECT a,b FROM t WHERE a=50;
ROLLBACK TO s4;             -- must restore 'fifty-first', not delete row 50
SELECT a,b FROM t WHERE a=50;
ROLLBACK TO s3;             -- now it goes away
SELECT count(*) FROM t WHERE a=50;
RELEASE s3;
COMMIT;
SELECT count(*), max(a) FROM t;

-- 4. three versions of a key above the bound, unwound one savepoint at a time
BEGIN;
SELECT a FROM t WHERE a<=999 ORDER BY a DESC LIMIT 1;
INSERT INTO t VALUES(60,'v1');
SAVEPOINT sa;
UPDATE t SET b='v2' WHERE a=60;
SAVEPOINT sb;
UPDATE t SET b='v3' WHERE a=60;
SELECT b FROM t WHERE a=60;
ROLLBACK TO sb;
SELECT b FROM t WHERE a=60;
ROLLBACK TO sa;
SELECT b FROM t WHERE a=60;
RELEASE sa;
ROLLBACK;
SELECT count(*) FROM t WHERE a=60;

-- 5. a whole-transaction rollback with writes above and below the bound
BEGIN;
SELECT a FROM t WHERE a<=999 ORDER BY a DESC LIMIT 1;
INSERT INTO t VALUES(70,'seventy');
UPDATE t SET b='TEN' WHERE a=10;
DELETE FROM t WHERE a=20;
SELECT a,b FROM t ORDER BY a;
ROLLBACK;
SELECT a,b FROM t ORDER BY a;

-- 6. a statement journal, with no savepoint anywhere: a UNIQUE violation
--    part-way through must undo that statement's earlier rows and leave the
--    rest of the transaction alone.
CREATE TABLE u(k INTEGER PRIMARY KEY, v TEXT UNIQUE);
INSERT INTO u VALUES(1,'one'),(2,'two');
CREATE TABLE feed(k INTEGER, v TEXT);
INSERT INTO feed VALUES(10,'ten'),(11,'eleven'),(12,'two'),(13,'thirteen');
BEGIN;
INSERT INTO u VALUES(5,'five');
SELECT count(*) FROM u;
INSERT INTO u(k,v) SELECT k,v FROM feed ORDER BY k;   -- row 12 collides
SELECT k,v FROM u ORDER BY k;      -- 10 and 11 must be gone, 5 must remain
COMMIT;
SELECT k,v FROM u ORDER BY k;

-- 7. the same, but the transaction is rolled back afterwards
BEGIN;
INSERT INTO u VALUES(6,'six');
INSERT INTO u(k,v) SELECT k,v FROM feed ORDER BY k;
SELECT k,v FROM u ORDER BY k;
ROLLBACK;
SELECT k,v FROM u ORDER BY k;

-- 8. index trees: a WITHOUT ROWID table and a secondary index, same
--    questions.  The bound is per tree, so each has its own.
CREATE TABLE w(k TEXT PRIMARY KEY, v INT) WITHOUT ROWID;
INSERT INTO w VALUES('aaa',1),('bbb',2),('ccc',3);
CREATE INDEX wv ON w(v);
BEGIN;
SELECT k FROM w WHERE k<='zzz' ORDER BY k DESC LIMIT 1;
SAVEPOINT s8;
INSERT INTO w VALUES('ddd',4);
UPDATE w SET v=99 WHERE k='aaa';
SELECT k,v FROM w ORDER BY k;
SELECT k FROM w WHERE v=99;
ROLLBACK TO s8;
SELECT k,v FROM w ORDER BY k;
SELECT count(*) FROM w WHERE v=99;
SELECT k FROM w WHERE v=1;
RELEASE s8;
COMMIT;
SELECT k,v FROM w ORDER BY k;

-- 9. a savepoint spanning inserts into two trees at once, rolled back
BEGIN;
SAVEPOINT s9;
INSERT INTO t VALUES(80,'eighty');
INSERT INTO w VALUES('eee',5);
SELECT max(a) FROM t;
SELECT max(k) FROM w;
ROLLBACK TO s9;
SELECT max(a) FROM t;
SELECT max(k) FROM w;
RELEASE s9;
COMMIT;
SELECT count(*) FROM t;
SELECT count(*) FROM w;
PRAGMA integrity_check;

-- 10. THE HINT PATH.  From here down, the before-image comes from a cursor
--     that is already sitting on the row being overwritten instead of from
--     a fetch (zsbtUndoHint).  A point UPDATE by primary key is the shape:
--     OP_SeekRowid positions and reads the row, then the write reuses it.
BEGIN;
SAVEPOINT s10;
UPDATE t SET b='TWENTY' WHERE a=20;
SELECT a,b FROM t WHERE a=20;
ROLLBACK TO s10;
SELECT a,b FROM t WHERE a=20;
RELEASE s10;
COMMIT;
SELECT a,b FROM t ORDER BY a;

-- 11. An auto-rowid INSERT: OP_NewRowid leaves the cursor on the LARGEST
--     key while the insert goes to a new one, so a hint that skipped its
--     key comparison would save the wrong row's value here and a rollback
--     would leave the new row behind holding it.
BEGIN;
SAVEPOINT s11;
INSERT INTO t(b) VALUES('auto');
SELECT count(*) FROM t;
SELECT b FROM t ORDER BY a DESC LIMIT 1;
ROLLBACK TO s11;
SELECT count(*) FROM t;
SELECT a,b FROM t ORDER BY a;
RELEASE s11;
COMMIT;
SELECT count(*), max(a) FROM t;

-- 12. The same row updated repeatedly, each version behind its own
--     savepoint: every write after the first must see the version the
--     write before it left, not the one the cursor loaded originally.
BEGIN;
UPDATE t SET b='r0' WHERE a=10;
SAVEPOINT sx;
UPDATE t SET b='r1' WHERE a=10;
SAVEPOINT sy;
UPDATE t SET b='r2' WHERE a=10;
SELECT b FROM t WHERE a=10;
ROLLBACK TO sy;
SELECT b FROM t WHERE a=10;
ROLLBACK TO sx;
SELECT b FROM t WHERE a=10;
RELEASE sx;
ROLLBACK;
SELECT b FROM t WHERE a=10;

-- 13. a point DELETE, where the cursor is guaranteed to be on the row
BEGIN;
SAVEPOINT s13;
DELETE FROM t WHERE a=30;
SELECT count(*) FROM t WHERE a=30;
ROLLBACK TO s13;
SELECT a,b FROM t WHERE a=30;
RELEASE s13;
COMMIT;
SELECT count(*) FROM t;

-- 14. an UPDATE that MOVES a rowid: a delete of the cursor's key followed
--     by an insert of a key no cursor is on
BEGIN;
SAVEPOINT s14;
UPDATE t SET a=a+1000 WHERE a=20;
SELECT a,b FROM t ORDER BY a;
ROLLBACK TO s14;
SELECT a,b FROM t ORDER BY a;
RELEASE s14;
COMMIT;
SELECT count(*), max(a) FROM t;

-- 15. the index-tree flavour: a WITHOUT ROWID point update, whose write is
--     an index replace, plus a secondary index rolled back with it
BEGIN;
SAVEPOINT s15;
UPDATE w SET v=v*10 WHERE k='bbb';
SELECT k,v FROM w ORDER BY k;
SELECT k FROM w WHERE v=20;
ROLLBACK TO s15;
SELECT k,v FROM w ORDER BY k;
SELECT count(*) FROM w WHERE v=20;
SELECT k FROM w WHERE v=2;
RELEASE s15;
COMMIT;
SELECT k,v FROM w ORDER BY k;
PRAGMA integrity_check;
