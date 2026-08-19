-- Rowid-table seeks, with the cursor position after a MISS as the subject.
-- sqlite3BtreeTableMoveto returns *pRes<0 meaning "positioned on an entry
-- smaller than the key sought", and the callers do four different things
-- with that: OP_SeekGE/GT step forward, OP_SeekLE/LT read the row where
-- they stand, OP_NotExists jumps and never looks, and OP_NewRowid only
-- asks whether anything was there.  Deferring the positioning fetch has
-- to satisfy all four, including after deletes and a rollback have left
-- the engine's append bound stale-high.

CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);
INSERT INTO t SELECT i*10, 'row '||(i*10)
  FROM (WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<50)
        SELECT i FROM c);
SELECT count(*), min(a), max(a) FROM t;

-- exact hits, gaps, and past the end (OP_SeekRowid / OP_NotExists)
SELECT a, b FROM t WHERE a=10;
SELECT a, b FROM t WHERE a=250;
SELECT a, b FROM t WHERE a=500;
SELECT count(*) FROM t WHERE a=255;      -- in a gap
SELECT count(*) FROM t WHERE a=501;      -- past the max
SELECT count(*) FROM t WHERE a=999999;   -- far past the max
SELECT count(*) FROM t WHERE a=0;        -- below the min
SELECT count(*) FROM t WHERE a=-7;       -- negative, below everything

-- forward seeks that step after landing below (OP_SeekGE / OP_SeekGT)
SELECT a FROM t WHERE a>=255 ORDER BY a LIMIT 3;
SELECT a FROM t WHERE a>255  ORDER BY a LIMIT 3;
SELECT a FROM t WHERE a>=500 ORDER BY a;
SELECT a FROM t WHERE a>=501 ORDER BY a;         -- empty: nothing at or above
SELECT a FROM t WHERE a>500  ORDER BY a;         -- empty
SELECT count(*) FROM t WHERE a>=0;
SELECT a FROM t WHERE a>=-100 ORDER BY a LIMIT 2;

-- backward seeks that READ where they land, without stepping
-- (OP_SeekLE / OP_SeekLT: this is the case a deferred position must
-- resolve to the entry BELOW the key, not to its successor)
SELECT a, b FROM t WHERE a<=255 ORDER BY a DESC LIMIT 3;
SELECT a, b FROM t WHERE a<255  ORDER BY a DESC LIMIT 3;
SELECT a, b FROM t WHERE a<=501 ORDER BY a DESC LIMIT 2;
SELECT a, b FROM t WHERE a<=999999 ORDER BY a DESC LIMIT 1;
SELECT a, b FROM t WHERE a<10 ORDER BY a DESC;   -- empty: nothing below
SELECT a, b FROM t WHERE a<=9 ORDER BY a DESC;   -- empty

-- a seek past the end then a step backwards, and vice versa
SELECT a FROM t WHERE a<=1000 ORDER BY a DESC LIMIT 3;
SELECT max(a) FROM t;
SELECT min(a) FROM t;

-- deletes leave the append bound stale-high: the true max drops, the
-- bound does not, so every seek above the new max takes the shortcut
-- with a bound that no longer names a live row
DELETE FROM t WHERE a>400;
SELECT count(*), max(a) FROM t;
SELECT count(*) FROM t WHERE a=450;
SELECT a FROM t WHERE a>=405 ORDER BY a;         -- empty
SELECT a, b FROM t WHERE a<=450 ORDER BY a DESC LIMIT 2;
SELECT a, b FROM t WHERE a<=999999 ORDER BY a DESC LIMIT 1;

-- inserts inside a transaction raise the bound as they go, and seeks
-- interleave with them
BEGIN;
INSERT INTO t VALUES(1000,'thousand');
SELECT a FROM t WHERE a>=500 ORDER BY a;
SELECT a, b FROM t WHERE a<=1500 ORDER BY a DESC LIMIT 1;
INSERT INTO t VALUES(2000,'two thousand');
SELECT count(*) FROM t WHERE a=1500;
SELECT a FROM t WHERE a>1000 ORDER BY a;
COMMIT;
SELECT count(*), max(a) FROM t;

-- a rollback lowers the true max while the bound stays where the
-- rolled-back insert left it
BEGIN;
INSERT INTO t VALUES(5000,'five thousand');
SELECT max(a) FROM t;
ROLLBACK;
SELECT max(a) FROM t;
SELECT count(*) FROM t WHERE a=5000;
SELECT a FROM t WHERE a>=2001 ORDER BY a;        -- empty
SELECT a, b FROM t WHERE a<=5000 ORDER BY a DESC LIMIT 1;

-- savepoint rollback, same question one level down
SAVEPOINT s;
INSERT INTO t VALUES(9000,'nine thousand');
SELECT max(a) FROM t;
ROLLBACK TO s;
RELEASE s;
SELECT max(a) FROM t;
SELECT count(*) FROM t WHERE a=9000;
SELECT a, b FROM t WHERE a<=9000 ORDER BY a DESC LIMIT 1;

-- implicit rowid allocation (OP_NewRowid) after all of that, mixed with
-- explicit keys above and below the current maximum
INSERT INTO t(b) VALUES('auto one');
INSERT INTO t(b) VALUES('auto two');
SELECT a, b FROM t ORDER BY a DESC LIMIT 3;
INSERT INTO t VALUES(2500,'explicit above');
INSERT INTO t VALUES(15,'explicit below');
SELECT a, b FROM t ORDER BY a LIMIT 3;
SELECT a, b FROM t ORDER BY a DESC LIMIT 3;

-- UPDATE and DELETE by rowid, which go through the same seek then write
UPDATE t SET b='updated' WHERE a=2500;
SELECT a, b FROM t WHERE a=2500;
UPDATE t SET b='ignored' WHERE a=2501;
SELECT changes();
DELETE FROM t WHERE a=15;
SELECT count(*) FROM t WHERE a=15;
DELETE FROM t WHERE a=99999;
SELECT changes();

-- a correlated subquery: one rowid seek per outer row, half of them misses
SELECT count(*) FROM t x WHERE EXISTS(SELECT 1 FROM t y WHERE y.a=x.a+10);
SELECT sum(a) FROM t;

-- SEEKS INSIDE A WRITE TRANSACTION.  The engine's append bound only
-- exists in a write transaction, so this is the ONLY place a shortcut
-- keyed on it can fire -- everything above runs in autocommit, where a
-- read-only SELECT gets a read transaction and no bound at all.  A
-- mutation check found the section above catching a broken shortcut on
-- one line out of sixty-seven, which is what this section is for.
BEGIN;
SELECT max(a), min(a), count(*) FROM t;
SELECT a, b FROM t WHERE a<=999999 ORDER BY a DESC LIMIT 1;   -- LE, far above
SELECT a, b FROM t WHERE a<999999  ORDER BY a DESC LIMIT 2;   -- LT, far above
SELECT a, b FROM t WHERE a<=2500   ORDER BY a DESC LIMIT 1;   -- LE, at the max
SELECT a, b FROM t WHERE a<2500    ORDER BY a DESC LIMIT 1;   -- LT, at the max
SELECT a, b FROM t WHERE a<=2499   ORDER BY a DESC LIMIT 1;   -- LE, just below
SELECT a FROM t WHERE a>=2501 ORDER BY a;                     -- GE above: empty
SELECT a FROM t WHERE a>2500  ORDER BY a;                     -- GT at max: empty
SELECT a FROM t WHERE a>=2500 ORDER BY a;                     -- GE at the max
SELECT count(*) FROM t WHERE a=3000;                          -- NotExists above
SELECT count(*) FROM t WHERE a=2500;                          -- NotExists at max
-- an insert raises the bound, so the same seeks answer differently
INSERT INTO t VALUES(4000,'four thousand');
SELECT a, b FROM t WHERE a<=999999 ORDER BY a DESC LIMIT 1;
SELECT a FROM t WHERE a>=2501 ORDER BY a;
SELECT a, b FROM t WHERE a<=4000 ORDER BY a DESC LIMIT 2;
COMMIT;

-- ...and with the table emptied inside the transaction, where the bound
-- still names a key and nothing is below it: the case that cannot be
-- answered from the bound alone.
BEGIN;
SELECT count(*) FROM t;
DELETE FROM t;
SELECT count(*) FROM t;
SELECT a, b FROM t WHERE a<=999999 ORDER BY a DESC LIMIT 1;   -- empty
SELECT a, b FROM t WHERE a<=1 ORDER BY a DESC LIMIT 1;        -- empty
SELECT a FROM t WHERE a>=0 ORDER BY a LIMIT 1;                -- empty
SELECT count(*) FROM t WHERE a=2500;
SELECT max(a), min(a) FROM t;
INSERT INTO t VALUES(77,'after the purge');
SELECT a, b FROM t WHERE a<=999999 ORDER BY a DESC LIMIT 1;
SELECT a, b FROM t WHERE a<=76 ORDER BY a DESC LIMIT 1;       -- empty
SELECT a FROM t WHERE a>=78 ORDER BY a;                       -- empty
ROLLBACK;
SELECT count(*), max(a) FROM t;

PRAGMA integrity_check;
