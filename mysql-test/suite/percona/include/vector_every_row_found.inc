# Search t1's vector index once from every row's own vector, and count the
# rows it does not find at distance 0. Each search is a top-level
# ORDER BY DISTANCE ... LIMIT 1, the shape that uses the vector index.
# Needs table t1 with VECTOR(4) column v and vector index vk.
--disable_query_log
# A wide search, so a row the index does not return is a missing node, not
# the approximation HNSW is allowed at the default breadth.
SET @vector_saved_ef = @@session.innodb_hnsw_ef_search;
SET SESSION innodb_hnsw_ef_search = 400;
DELIMITER |;
CREATE PROCEDURE vector_every_row_found()
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE qv VARCHAR(200);
  DECLARE c CURSOR FOR SELECT VECTOR_TO_STRING(v) FROM t1 FORCE INDEX (PRIMARY);
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;
  SET @missing = 0, @searched = 0;
  OPEN c;
  scan: LOOP
    FETCH c INTO qv;
    IF done THEN LEAVE scan; END IF;
    SELECT DISTANCE(v, STRING_TO_VECTOR(qv), 'euclidean') INTO @d
      FROM t1 ORDER BY DISTANCE(v, STRING_TO_VECTOR(qv), 'euclidean') LIMIT 1;
    SET @searched = @searched + 1;
    IF @d <> 0 THEN SET @missing = @missing + 1; END IF;
  END LOOP;
  CLOSE c;
END|
DELIMITER ;|
CALL vector_every_row_found();
DROP PROCEDURE vector_every_row_found;
SELECT @searched = (SELECT COUNT(*) FROM t1 FORCE INDEX (PRIMARY)) AS all_rows_searched,
       @missing AS rows_not_found;
SET SESSION innodb_hnsw_ef_search = @vector_saved_ef;
--enable_query_log
