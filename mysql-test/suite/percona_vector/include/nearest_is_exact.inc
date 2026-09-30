# Compare, for five query vectors, the distance of the nearest row the vector
# index returns with the smallest distance a full scan finds. Equal distances,
# not equal ids: rows tied at the minimum make either answer correct.
# Needs table t1 with VECTOR(4) column v and vector index vk.
--disable_query_log
let $q = 1;
while ($q < 6)
{
  eval SELECT DISTANCE(v, STRING_TO_VECTOR('[$q,1,1,1]'), 'euclidean') INTO @ann
         FROM t1 ORDER BY DISTANCE(v, STRING_TO_VECTOR('[$q,1,1,1]'), 'euclidean') LIMIT 1;
  eval SELECT MIN(DISTANCE(v, STRING_TO_VECTOR('[$q,1,1,1]'), 'euclidean')) INTO @exact
         FROM t1 IGNORE INDEX (vk);
  eval SELECT $q AS query, @ann = @exact AS same_nearest;
  inc $q;
}
--enable_query_log
