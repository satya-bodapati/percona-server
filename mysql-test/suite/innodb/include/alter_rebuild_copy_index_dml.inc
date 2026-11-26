#
# DML on $tbl that leaves delete-marked and new entries in each secondary
# index.
#
--echo # DML on $tbl.
--disable_query_log
--echo # Key updates. The old entries are delete-marked.
--eval UPDATE $tbl SET a = a + 1, b = CONCAT('z', b), u = u + 100000, t = CONCAT('Z', t) WHERE id BETWEEN 1000 AND 1500
--echo # Deletes.
--eval DELETE FROM $tbl WHERE id BETWEEN 2000 AND 2300
--echo # Deletes and inserts of the same primary keys with other keys.
--eval DELETE FROM $tbl WHERE id BETWEEN 3000 AND 3100
--eval INSERT INTO $tbl WITH RECURSIVE seq(n) AS (SELECT 3000 UNION ALL SELECT n + 1 FROM seq WHERE n < 3100) SELECT n, 999 - n % 1000, CONCAT('m', n), n + 300000, CONCAT('M', n) FROM seq
--echo # Unique keys set to NULL and from NULL.
--eval UPDATE $tbl SET u = NULL WHERE id BETWEEN 4000 AND 4100
--eval UPDATE $tbl SET u = id + 200000 WHERE u IS NULL AND id BETWEEN 5000 AND 5100
--echo # A key update to a value that is equal in the collation.
--eval UPDATE $tbl SET b = UPPER(b) WHERE id BETWEEN 6000 AND 6100
--echo # Rows with an off-page column whose index entry is unchanged.
--eval UPDATE $tbl SET t = CONCAT(t, 'q') WHERE id % 7 = 0 AND id % 500 <> 0
--eval UPDATE $tbl SET a = a + 2 WHERE id % 500 = 0
--echo # New rows.
--eval INSERT INTO $tbl WITH RECURSIVE seq(n) AS (SELECT 30001 UNION ALL SELECT n + 1 FROM seq WHERE n < 30500) SELECT n, n % 7, CONCAT('k', n), IF(n % 2, NULL, n), CONCAT('L', n) FROM seq
--enable_query_log
