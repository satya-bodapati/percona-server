# Shared body for the vector_mvcc_concurrent_* tests. The caller sets $mutation
# (one statement another session runs) and $label.
#
# The search is made fragile on purpose:
#
#   M = 2                    the sparsest graph the engine allows
#   ef_search = 1, LIMIT 1   effective width max(1,1) = 1: a pure greedy walk
#                            with no candidate list behind it
#   64 dimensions            recall is genuinely imperfect, so which path the
#                            walk takes decides the answer
#
# These settings are for speed and reliability of reproduction, NOT because the
# behaviour needs provoking. Measured at ordinary ones - 384-dimension
# embeddings, M=16, the shipped default ef_search of 40, LIMIT 10 over 2000
# rows - a concurrent INSERT still changed what one of five query vectors
# returned inside a single snapshot: row 620 left the top ten and row 940 took
# its place, both rows the snapshot could see throughout. Real embeddings are
# high-dimensional, which is exactly where ANN recall is below 1 and the path
# decides the answer. A small, low-dimensional table hides this; it is the
# unrepresentative case, not the safe one.
#
# Ten query vectors are read before and after the writer, inside one snapshot.
# Their row ids are never echoed: the layer each node lands on is drawn at
# random per build, so the ids differ between runs. What is stable, and what
# these tests assert, is whether the two reads agree.
SET SESSION cte_max_recursion_depth = 100000;
--disable_query_log
CREATE TABLE t (id BIGINT UNSIGNED PRIMARY KEY, v VECTOR(64) NOT NULL,
                VECTOR KEY vk (v) TYPE hnsw (M = 2));
INSERT INTO t (id, v)
WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM s WHERE n < 2000),
     d(k) AS (SELECT 1 UNION ALL SELECT k+1 FROM d WHERE k < 64)
SELECT n, STRING_TO_VECTOR(CONCAT('[',
       (SELECT GROUP_CONCAT(ROUND(RAND(n*64+k)*100,3)) FROM d), ']')) FROM s;
--enable_query_log

--echo # Nodes in the graph before the writer:
SET SESSION innodb_interpreter = 'vec_aux_verify test/t';
SELECT @@session.innodb_interpreter_output;

--echo # The reads below are on the ANN path - without that this tests nothing.
--replace_column 10 # 11 #
EXPLAIN SELECT id FROM t
  ORDER BY DISTANCE(v, STRING_TO_VECTOR(CONCAT('[', TRIM(TRAILING ',' FROM REPEAT('10,',64)), ']')), 'EUCLIDEAN') LIMIT 1;

connect (reader, localhost, root,,);
connection reader;
SET SESSION innodb_hnsw_ef_search = 1;
SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ;
START TRANSACTION;
SELECT COUNT(*) AS rows_in_snapshot FROM t;
--disable_query_log
--let $before =
--let $i = 0
while ($i < 10) {
  --let $val = `SELECT $i * 10`
  --let $r = `SELECT id FROM t ORDER BY DISTANCE(v, STRING_TO_VECTOR(CONCAT('[', TRIM(TRAILING ',' FROM REPEAT(CONCAT($val,','),64)), ']')), 'EUCLIDEAN') LIMIT 1`
  --let $before = $before $r
  --inc $i
}
--enable_query_log

connection default;
--disable_query_log
eval $mutation;
--enable_query_log
SELECT COUNT(*) AS rows_the_writer_left FROM t;
--echo # Nodes in the graph after the writer:
SET SESSION innodb_interpreter = 'vec_aux_verify test/t';
SELECT @@session.innodb_interpreter_output;

connection reader;
--disable_query_log
--let $after =
--let $i = 0
while ($i < 10) {
  --let $val = `SELECT $i * 10`
  --let $r = `SELECT id FROM t ORDER BY DISTANCE(v, STRING_TO_VECTOR(CONCAT('[', TRIM(TRAILING ',' FROM REPEAT(CONCAT($val,','),64)), ']')), 'EUCLIDEAN') LIMIT 1`
  --let $after = $after $r
  --inc $i
}
--let $same = `SELECT '$before' = '$after'`
--enable_query_log

--echo # The snapshot still counts exactly what it opened on:
SELECT COUNT(*) AS rows_in_snapshot FROM t;
--echo # ...and nothing the writer created is reachable from inside it:
SELECT COUNT(*) AS leaked_new_rows FROM
  (SELECT id FROM t
     ORDER BY DISTANCE(v, STRING_TO_VECTOR(CONCAT('[', TRIM(TRAILING ',' FROM REPEAT('50,',64)), ']')), 'EUCLIDEAN')
     LIMIT 50) x
  WHERE id > 1000000;
--echo #
--echo # Ten query vectors, read before the writer and again after it, in one
--echo # REPEATABLE READ transaction. Do the two reads agree?
--echo #
if ($same) {
  --echo   $label: the two reads AGREE
}
if (!$same) {
  --echo   $label: the two reads DIFFER
}
COMMIT;
disconnect reader;
connection default;
DROP TABLE t;
