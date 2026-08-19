CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);
INSERT INTO t VALUES(1,'one'),(2,'two'),(3,'three');
CREATE INDEX tb ON t(b);
CREATE TABLE u(x UNIQUE);
INSERT INTO u VALUES(10),(20);
.backup main /tmp/zs-backup-dest
.open /tmp/zs-backup-dest
SELECT a,b FROM t ORDER BY a;
SELECT a FROM t WHERE b='two';
SELECT x FROM u ORDER BY x;
INSERT INTO t VALUES(4,'four');
SELECT count(*) FROM t;
VACUUM;
SELECT count(*) FROM t;
SELECT a FROM t WHERE b='four';
PRAGMA integrity_check;
