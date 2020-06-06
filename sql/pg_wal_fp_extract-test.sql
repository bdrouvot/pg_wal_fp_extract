select setting as pgdata from pg_settings where name = 'data_directory'
\gset

create table bdt (a int);
insert into bdt select * from generate_series(1,5);

select pg_relation_filepath('bdt') as relpath
\gset

\o ./extract_relation_block.sh
\qecho dd if=:pgdata/:relpath  of=from_relation.dump bs=8192 count=1 seek=0 skip=0 status=none
\o

select oid as bdtoid from pg_class where relname = 'bdt'
\gset

checkpoint;
insert into bdt values (6);

\o ./extract_walblock.sh
select pg_walfile_name(pg_current_wal_lsn()) as walp
\gset
\qecho ./pg_wal_fp_extract :pgdata/pg_wal/:walp -r :bdtoid -c
\o

\! rm /tmp/tbs*.dump
\! echo ""
\! echo "Extracting the Full Page from the WAL..."
\! cat ./extract_walblock.sh
\! /bin/sh ./extract_walblock.sh
\! echo ""

checkpoint;
\! echo "Extracting the block from the relation..."
\! cat ./extract_relation_block.sh
\! /bin/sh ./extract_relation_block.sh
\! echo ""

\! cp -p /tmp/tbs*dump ./from_wal.dump
\! hexdump ./from_wal.dump > ./hex_from_wal.hex
\! hexdump from_relation.dump > ./from_relation.hex
\! echo "Comparing wal extracted block and relation block..."
\! diff -s ./hex_from_wal.hex ./from_relation.hex

\! tools/heap_page_items_info.sh -b 0 -p ./from_wal.dump -i disk
\! tools/heap_page_items_info.sh -b 0 -p ./from_relation.dump -i disk

\! tools/page_header_info.sh -b 0 -p ./from_wal.dump -i disk
\! tools/page_header_info.sh -b 0 -p ./from_relation.dump -i disk

\! rm extract_relation_block.sh
\! rm from_relation.dump
\! rm extract_walblock.sh
\! rm from_wal.dump
\! rm hex_from_wal.hex
\! rm from_relation.hex
\! echo ""
drop table bdt;
