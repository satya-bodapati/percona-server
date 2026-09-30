# Run $alter on t1 with $crash armed, DML from con1 while it waits after
# its scan, then let it crash in its commit and restart. Afterwards print
# what must hold whichever way recovery resolved the ALTER. Set $first_add
# when $alter adds the first vector index: recovery then leaves none, and
# the ALTER is run again before the checks.
--echo # $alter, crashing at $crash
--connect (con1, localhost, root,,)
--connection default
SET DEBUG_SYNC = 'row_log_table_apply1_before SIGNAL built WAIT_FOR go';
eval SET SESSION DEBUG = '+d,$crash';
--send_eval $alter
--connection con1
SET DEBUG_SYNC = 'now WAIT_FOR built';
--disable_query_log
SELECT MAX(k) + 1 INTO @k FROM t1;
--enable_query_log
INSERT INTO t1 (k, v) VALUES (@k, STRING_TO_VECTOR(CONCAT('[', @k, ',2,0,0]')));
INSERT INTO t1 (k, v) VALUES (@k + 1, STRING_TO_VECTOR(CONCAT('[0,', @k, ',2,0]')));
UPDATE t1 SET v = STRING_TO_VECTOR(CONCAT('[0,0,', @k, ',2]')) WHERE k = 2;
--source include/expect_crash.inc
SET DEBUG_SYNC = 'now SIGNAL go';
--disconnect con1
--connection default
--error CR_SERVER_LOST
--reap
--source include/start_mysqld.inc
--echo # The DML logged during the ALTER is in the table.
SELECT COUNT(*) FROM t1 FORCE INDEX (PRIMARY) WHERE k >= 7;
if ($first_add)
{
  --echo # Recovery undid the first ADD: no label column, no index. It runs
  --echo # again, online, over the rows the logged DML left.
  SELECT COUNT(*) AS label_column FROM INFORMATION_SCHEMA.INNODB_COLUMNS c
    JOIN INFORMATION_SCHEMA.INNODB_TABLES t USING (TABLE_ID)
   WHERE t.NAME = 'test/t1' AND c.NAME = 'percona_vec_aux_id';
  eval $alter;
}
SET SESSION innodb_interpreter = 'vec_aux_verify test/t1';
SELECT @@session.innodb_interpreter_output LIKE '% dup_ids=0 %' AS no_shared_label;
--source suite/percona_vector/include/every_row_found.inc
--echo # The next rows take new labels and new AUTO_INCREMENT ids.
INSERT INTO t1 (k, v) VALUES (100, STRING_TO_VECTOR('[9,9,9,9]')),
                             (101, STRING_TO_VECTOR('[9,9,9,8]'));
SET SESSION innodb_interpreter = 'vec_aux_verify test/t1';
SELECT @@session.innodb_interpreter_output LIKE '% dup_ids=0 %' AS no_shared_label;
--source suite/percona_vector/include/every_row_found.inc
SELECT COUNT(DISTINCT id) = COUNT(*) AS ids_unique FROM t1 FORCE INDEX (PRIMARY);
DELETE FROM t1 WHERE k IN (100, 101);
