# Check that the reset block of a page tracking file has a reset point whose
# data block carries a reset LSN.
#
# Parameters:
#   $reset_file_index      index k of the file ib_page_k, or "any" to search
#                          every file of the group
#   $reset_lsn             reset LSN to find
#   $data_blocks_per_file  data blocks in a file: 2043, or 7 with
#                          page_archiver_simulate_more_archived_files

let PT_CHECK_FILE_INDEX = $reset_file_index;
let PT_CHECK_RESET_LSN = $reset_lsn;
let PT_CHECK_BLOCKS_PER_FILE = $data_blocks_per_file;

--perl
use strict;
my $block_size = 16384;
my $index = $ENV{PT_CHECK_FILE_INDEX};
my $pattern = $index eq 'any' ? '*' : $index;
my @paths = glob("$ENV{MYSQLD_DATADIR}/#ib_archive/page_group_*/ib_page_$pattern");
die "There is no ib_page_$index\n" unless @paths;
my $found = 0;
foreach my $path (@paths) {
  open(my $fh, '<:raw', $path) or die "Cannot open $path: $!\n";
  read($fh, my $reset_block, $block_size) == $block_size or next;
  # Header: data length at offset 6. Data: an 8-byte file LSN, then 2-byte
  # block number and 2-byte offset pairs.
  my $data_len = unpack('n', substr($reset_block, 6, 2));
  for (my $pos = 8; $pos + 4 <= $data_len; $pos += 4) {
    my ($block_num) = unpack('n', substr($reset_block, 32 + $pos, 2));
    my $block = $block_num % $ENV{PT_CHECK_BLOCKS_PER_FILE};
    seek($fh, ($block + 1) * $block_size, 0) or next;
    read($fh, my $header, 32) == 32 or next;
    # Reset LSN of the data block at offset 16.
    $found = 1 if unpack('Q>', substr($header, 16, 8)) == $ENV{PT_CHECK_RESET_LSN};
  }
  close($fh);
}
die "No reset point in ib_page_$index has the reset LSN $ENV{PT_CHECK_RESET_LSN}\n"
  unless $found;
EOF
