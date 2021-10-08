pg_wal_fp_extract
===================

Features
--------

Extract Full Page(s) from postgres WAL

Installation
============

Compiling
---------

The utility can be built using the standard PGXS infrastructure. For this to
work, the ``pg_config`` program must be available in your $PATH. Instruction to
install follows:

    $ git clone
    $ cd pg_wal_fp_extract
    $ make

Usage
=======

```
$ ./pg_wal_fp_extract --help
pg_wal_fp_extract: Extract Full Page from write-ahead logs for debugging.

Usage:
  pg_wal_fp_extract [OPTION] <file_path>
  -s, --start=RECPTR start extracting at WAL location RECPTR
  -e, --end=RECPTR   stop extracting at WAL location RECPTR
  -x, --xid=XID      only extract from records with transaction ID XID
  -r, --rel=RID      only extract pages with relation ID RID
  -b, --blk=BLK      only extract block number BLK
  -d, --dest         destination directory for extracted block(s) (default /tmp)
  -c, --check        generate checksum in page dump (default false)
  -t, --test         dryrun (display only)
```

Example
=======

```
postgres=# create table bdt (a int);
CREATE TABLE

postgres=# insert into bdt select * from generate_series(1,5);
INSERT 0 5

postgres=# checkpoint;
CHECKPOINT

postgres=# insert into bdt values (6);
INSERT 0 1

Extracting the Full Page from the WAL with the utility...
$ ./pg_wal_fp_extract /usr/local/pgsql12.3-last/data/pg_wal/000000010000000000000001 -r 16466 -c

Dumping (compressed) FPI from rel 1663/13593/16466 fork main blk 0 from lsn 0/01A96D20 to /tmp/tbs_1663_db_13593_rel_16466_fork_main_blk_0_lsn_0_01A96D20.dump

postgres=# checkpoint;
CHECKPOINT

Extracting the block from the relation...
$ dd if=/usr/local/pgsql12.3-last/data/base/13593/16466 of=from_relation.dump bs=8192 count=1 seek=0 skip=0 status=none

$ mv /tmp/tbs_1663_db_13593_rel_16466_fork_main_blk_0_lsn_0_01A96D20.dump ./from_wal.dump
$ hexdump ./from_wal.dump > ./hex_from_wal.hex
$ hexdump from_relation.dump > ./from_relation.hex

Comparing wal extracted block and relation block...

$ diff -s ./hex_from_wal.hex ./from_relation.hex
$ Files ./hex_from_wal.hex and ./from_relation.hex are identical

$ heap_page_items_info.sh -b 0 -p ./from_wal.dump -i disk

BLOCK   = 0
PATH    = ./from_wal.dump
TABLE   = 
INSPECT = disk

from | lp | lp_off | lp_flags | lp_len | t_xmin | t_xmax | t_field3 | t_ctid              | t_infomask2 | t_infomask | t_hoff
-----+----+--------+----------+--------+--------+--------+----------+---------------------+-------------+------------+--------
disk | 1  | 8160   | 1        | 28     | 597    | 0      | 0        | ( 0        , 1    ) | 1           | 2048       | 24     
disk | 2  | 8128   | 1        | 28     | 597    | 0      | 0        | ( 0        , 2    ) | 1           | 2048       | 24     
disk | 3  | 8096   | 1        | 28     | 597    | 0      | 0        | ( 0        , 3    ) | 1           | 2048       | 24     
disk | 4  | 8064   | 1        | 28     | 597    | 0      | 0        | ( 0        , 4    ) | 1           | 2048       | 24     
disk | 5  | 8032   | 1        | 28     | 597    | 0      | 0        | ( 0        , 5    ) | 1           | 2048       | 24     
disk | 6  | 8000   | 1        | 28     | 598    | 0      | 0        | ( 0        , 6    ) | 1           | 2048       | 24     

$ heap_page_items_info.sh -b 0 -p ./from_relation.dump -i disk

BLOCK   = 0
PATH    = ./from_relation.dump
TABLE   = 
INSPECT = disk

from | lp | lp_off | lp_flags | lp_len | t_xmin | t_xmax | t_field3 | t_ctid              | t_infomask2 | t_infomask | t_hoff
-----+----+--------+----------+--------+--------+--------+----------+---------------------+-------------+------------+--------
disk | 1  | 8160   | 1        | 28     | 597    | 0      | 0        | ( 0        , 1    ) | 1           | 2048       | 24     
disk | 2  | 8128   | 1        | 28     | 597    | 0      | 0        | ( 0        , 2    ) | 1           | 2048       | 24     
disk | 3  | 8096   | 1        | 28     | 597    | 0      | 0        | ( 0        , 3    ) | 1           | 2048       | 24     
disk | 4  | 8064   | 1        | 28     | 597    | 0      | 0        | ( 0        , 4    ) | 1           | 2048       | 24     
disk | 5  | 8032   | 1        | 28     | 597    | 0      | 0        | ( 0        , 5    ) | 1           | 2048       | 24     
disk | 6  | 8000   | 1        | 28     | 598    | 0      | 0        | ( 0        , 6    ) | 1           | 2048       | 24     

$ page_header_info.sh -b 0 -p ./from_wal.dump -i disk

BLOCK   = 0
PATH    = ./from_wal.dump
TABLE   = 
INSPECT = disk

 from    |    lsn            | checksum    | flags        | lower        | upper        | special      | prune_xid     
---------+-------------------+-------------+--------------+--------------+--------------+--------------+-----------
disk     | 00000000/01A96DC8 | 30982       | 0            | 48           | 8000         | 8192         | 0          

$ page_header_info.sh -b 0 -p ./from_relation.dump -i disk

BLOCK   = 0
PATH    = ./from_relation.dump
TABLE   = 
INSPECT = disk

 from    |    lsn            | checksum    | flags        | lower        | upper        | special      | prune_xid     
---------+-------------------+-------------+--------------+--------------+--------------+--------------+-----------
disk     | 00000000/01A96DC8 | 30982       | 0            | 48           | 8000         | 8192         | 0          
```

Remarks
=======
* the utility is for testing/debugging  
* has been tested on 10.13, 11.8 and 12.3

License
=======

pg_wal_fp_extract is free software distributed under the PostgreSQL license.

Copyright (c) 2020, Bertrand Drouvot.
