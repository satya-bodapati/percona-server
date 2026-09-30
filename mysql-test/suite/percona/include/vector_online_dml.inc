# Run $alter on t1 in the default connection, paused at $sync (default:
# after the scan, before the log is applied), while con1 inserts, updates
# vectors and other columns, and deletes. Then check the rows, the graph
# and the aux. t1: id AUTO_INCREMENT, k INT, v VECTOR(4), vector index vk.
if (!$sync)
{
  --let $_sync = row_log_table_apply1_before
}
if ($sync)
{
  --let $_sync = $sync
}
--connection default
eval SET DEBUG_SYNC = '$_sync SIGNAL built WAIT_FOR go';
--send_eval $alter
--connection con1
SET DEBUG_SYNC = 'now WAIT_FOR built';
--disable_query_log
SELECT MAX(k) + 1 INTO @k FROM t1;
--enable_query_log
INSERT INTO t1 (k, v) VALUES (@k, STRING_TO_VECTOR(CONCAT('[', @k, ',1,0,0]')));
INSERT INTO t1 (k, v) VALUES (@k + 1, STRING_TO_VECTOR(CONCAT('[0,', @k, ',1,0]')));
UPDATE t1 SET v = STRING_TO_VECTOR(CONCAT('[0,0,', @k, ',1]')) WHERE k = 3;
UPDATE t1 SET k = k WHERE k = 4;
UPDATE t1 SET v = STRING_TO_VECTOR(CONCAT('[1,0,0,', @k, ']')) WHERE k = @k + 1;
DELETE FROM t1 WHERE k = 5;
INSERT INTO t1 (k, v) VALUES (5, STRING_TO_VECTOR(CONCAT('[', @k, ',', @k, ',1,1]')));
SET DEBUG_SYNC = 'now SIGNAL go';
--connection default
--reap
SET DEBUG_SYNC = 'RESET';
--disable_query_log
# @k as con1 had it: its two inserts took k = @k and @k + 1, the largest.
SELECT MAX(k) - 1 INTO @k FROM t1;
--enable_query_log
--echo # Each changed row is its own nearest neighbour.
SELECT k FROM t1 ORDER BY DISTANCE(v, STRING_TO_VECTOR(CONCAT('[', @k, ',1,0,0]')), 'euclidean') LIMIT 1;
SELECT k FROM t1 ORDER BY DISTANCE(v, STRING_TO_VECTOR(CONCAT('[0,0,', @k, ',1]')), 'euclidean') LIMIT 1;
SELECT k FROM t1 ORDER BY DISTANCE(v, STRING_TO_VECTOR(CONCAT('[1,0,0,', @k, ']')), 'euclidean') LIMIT 1;
SELECT k FROM t1 ORDER BY DISTANCE(v, STRING_TO_VECTOR(CONCAT('[', @k, ',', @k, ',1,1]')), 'euclidean') LIMIT 1;
SELECT COUNT(*) FROM t1 FORCE INDEX (PRIMARY);
SET SESSION innodb_interpreter = 'vec_aux_verify test/t1';
SELECT @@session.innodb_interpreter_output;
--source suite/percona/include/vector_nearest_is_exact.inc
