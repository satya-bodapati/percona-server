# Track pages until the page tracking file ib_page_<$file_index> exists.
#
# Each round starts a new reset point, changes every page of t1 and flushes
# them, so the pages are tracked again. t1 must have more pages than one
# archive block holds page IDs (2044), or the next reset attaches to the
# current one and no new pages are tracked.
#
# Usage:
#   --let $page_tracking_start_lsn = <LSN returned by the set(1) that started
#                                     the group; it names the group directory>
#   --let $file_index = <index of the file to wait for>
#   --source suite/innodb/include/page_tracking_write_until_file.inc

--let $page_tracking_file = $MYSQLD_DATADIR#ib_archive/page_group_$page_tracking_start_lsn/ib_page_$file_index
--let $page_tracking_found = 0
--let $page_tracking_rounds = 0

--disable_query_log
--disable_result_log
while (!$page_tracking_found)
{
  if ($page_tracking_rounds == 100)
  {
    --die ib_page_$file_index was not created
  }

  SELECT mysqlbackup_page_track_set(1);
  UPDATE t1 SET n = n + 1;
  SET GLOBAL innodb_buf_flush_list_now = ON;

  --let $page_tracking_found = `SELECT LOAD_FILE('$page_tracking_file') IS NOT NULL`
  --inc $page_tracking_rounds
}
--enable_result_log
--enable_query_log
