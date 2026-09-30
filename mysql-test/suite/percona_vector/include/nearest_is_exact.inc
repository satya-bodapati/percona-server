# Compare, for five query vectors, the distance of the nearest row the vector
# index returns with the smallest distance a full scan finds. Equal distances,
# not equal ids: rows tied at the minimum make either answer correct.
# Needs table t1 with VECTOR(4) column v and a vector index, named by
# $vk_name (default vk).
--disable_query_log
# A wide search, so a row the index does not return is a missing node, not
# the approximation HNSW is allowed at the default breadth.
SET @vector_saved_ef = @@session.innodb_hnsw_ef_search;
SET SESSION innodb_hnsw_ef_search = 400;
--let $_vk = vk
if ($vk_name)
{
  --let $_vk = $vk_name
}
let $q = 1;
while ($q < 6)
{
  eval SELECT DISTANCE(v, STRING_TO_VECTOR('[$q,1,1,1]'), 'euclidean') INTO @ann
         FROM t1 ORDER BY DISTANCE(v, STRING_TO_VECTOR('[$q,1,1,1]'), 'euclidean') LIMIT 1;
  eval SELECT MIN(DISTANCE(v, STRING_TO_VECTOR('[$q,1,1,1]'), 'euclidean')) INTO @exact
         FROM t1 IGNORE INDEX ($_vk);
  eval SELECT $q AS query, @ann = @exact AS same_nearest;
  inc $q;
}
SET SESSION innodb_hnsw_ef_search = @vector_saved_ef;
--enable_query_log
