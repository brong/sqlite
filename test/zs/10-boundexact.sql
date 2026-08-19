-- The append bound as a POSITION, not just as a proof of absence.
--
-- Positioning a cursor from the bound requires it to name a key that is
-- still there, so every way of removing or moving the tree's largest key
-- has to invalidate it: a delete, a delete that is part of an UPDATE
-- moving a rowid, a savepoint rollback that restores one, and emptying
-- the table altogether.  All of it inside BEGIN, because the bound only
-- exists inside a write transaction -- outside one these queries would
-- never reach the code under test.
--
-- The shape that discriminates is LE/LT with a key ABOVE the maximum:
-- that is the seek that reads the row it lands on without stepping, so a
-- bound naming a dead key surfaces as a deleted row coming back to life.

CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);
INSERT INTO t SELECT i*10, 'row '||(i*10)
  FROM (WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<20)
        SELECT i FROM c);

-- 1. delete the maximum, then ask for the row below something above it
BEGIN;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- seeds the bound at 200
DELETE FROM t WHERE a=200;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- must be 190, not 200
SELECT a, b FROM t WHERE a<9999  ORDER BY a DESC LIMIT 2;
SELECT count(*) FROM t WHERE a=200;
SELECT max(a) FROM t;
COMMIT;
SELECT max(a), count(*) FROM t;

-- 2. delete the maximum repeatedly, seeking above it each time
BEGIN;
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;
DELETE FROM t WHERE a=190;
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;
DELETE FROM t WHERE a=180;
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;
DELETE FROM t WHERE a=170;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 3;
COMMIT;
SELECT max(a), count(*) FROM t;

-- 3. an UPDATE that moves the maximum's rowid downward: the delete half
--    removes the bound's key and the insert half lands below it
BEGIN;
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;      -- bound = 160
UPDATE t SET a=5 WHERE a=160;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- must be 150
SELECT a, b FROM t ORDER BY a LIMIT 2;
SELECT count(*) FROM t WHERE a=160;
COMMIT;
SELECT max(a), min(a), count(*) FROM t;

-- 4. delete the maximum then insert a higher one: the bound must end up
--    naming the new row, not the deleted one and not the one between
BEGIN;
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;
DELETE FROM t WHERE a=150;
INSERT INTO t VALUES(300,'three hundred');
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- 300
DELETE FROM t WHERE a=300;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- 140
INSERT INTO t VALUES(400,'four hundred');
DELETE FROM t WHERE a=400;
INSERT INTO t VALUES(500,'five hundred');
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 2;
COMMIT;
SELECT max(a), count(*) FROM t;

-- 5. a savepoint rollback that brings the maximum back, and one that
--    takes a newer maximum away
BEGIN;
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;      -- bound = 500
SAVEPOINT s1;
DELETE FROM t WHERE a=500;
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;      -- 140
ROLLBACK TO s1;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- 500 is back
SAVEPOINT s2;
INSERT INTO t VALUES(600,'six hundred');
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;      -- 600
ROLLBACK TO s2;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- 500 again
RELEASE s1;
COMMIT;
SELECT max(a), count(*) FROM t;

-- 6. empty the table under a seeded bound
BEGIN;
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;
DELETE FROM t;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- empty
SELECT count(*) FROM t;
INSERT INTO t VALUES(42,'forty two');
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- 42
DELETE FROM t WHERE a=42;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;   -- empty again
ROLLBACK;
SELECT max(a), count(*) FROM t;

-- 7. deletes BELOW the maximum must not disturb it
BEGIN;
SELECT a FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;
DELETE FROM t WHERE a IN (10,20,30);
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;
SELECT count(*) FROM t;
ROLLBACK;
SELECT count(*) FROM t;

-- 8. the same questions for an index tree, whose shortcut uses the bound
--    to prove absence rather than to position: an exact bound must not
--    change any answer here either
CREATE TABLE u(k TEXT PRIMARY KEY, v INT) WITHOUT ROWID;
INSERT INTO u SELECT 'key'||printf('%03d', i*10), i
  FROM (WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<20)
        SELECT i FROM c);
BEGIN;
SELECT k FROM u WHERE k<='zzz' ORDER BY k DESC LIMIT 1;
DELETE FROM u WHERE k='key200';
SELECT k FROM u WHERE k<='zzz' ORDER BY k DESC LIMIT 1;
INSERT INTO u VALUES('key999',99);
SELECT k FROM u WHERE k<='zzz' ORDER BY k DESC LIMIT 2;
SELECT count(*) FROM u WHERE k='key200';
SELECT count(*) FROM u WHERE k='key999';
DELETE FROM u WHERE k='key999';
SELECT k FROM u WHERE k<='zzz' ORDER BY k DESC LIMIT 1;
COMMIT;
SELECT count(*), max(k) FROM u;

-- 9. a secondary index on a rowid table: two trees, two bounds, one
--    statement touching both
CREATE INDEX tb ON t(b);
BEGIN;
INSERT INTO t VALUES(1000,'aaa');
INSERT INTO t VALUES(1010,'zzz');
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;
SELECT a FROM t WHERE b='zzz';
DELETE FROM t WHERE a=1010;
SELECT a, b FROM t WHERE a<=9999 ORDER BY a DESC LIMIT 1;
SELECT count(*) FROM t WHERE b='zzz';
SELECT b FROM t ORDER BY b DESC LIMIT 2;
COMMIT;
SELECT count(*), max(a) FROM t;
SELECT b FROM t ORDER BY b LIMIT 2;
PRAGMA integrity_check;
