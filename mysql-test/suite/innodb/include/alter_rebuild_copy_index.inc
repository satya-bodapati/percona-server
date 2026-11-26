#
# Read each secondary index of $tbl on its own: a covering checksum, and
# range reads through the prefix indexes.
#
--eval SELECT (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a)))) FROM $tbl FORCE INDEX (ka)) AS ka, (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a)))) FROM $tbl FORCE INDEX (kb)) AS kb, (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, IFNULL(u, -1))))) FROM $tbl FORCE INDEX (ku)) AS ku, (SELECT COUNT(*) FROM $tbl FORCE INDEX (kb) WHERE b BETWEEN 'k' AND 'p') AS kb_range, (SELECT COUNT(*) FROM $tbl FORCE INDEX (kt) WHERE t BETWEEN 'K' AND 'P') AS kt_range, (SELECT COUNT(*) FROM $tbl FORCE INDEX (ku) WHERE u IS NULL) AS ku_null
