#!/bin/bash

rm /mnt/* -rf

./db_bench_downcompact \
--benchmarks="ycsbfilldb,stats,ycsbwklda,stats" \
--db=/mnt/rocksdb_da \
--wal_dir=/mnt/rocksdb_wal \
--loadnum=50000000 \
--readwritepercent=0 \
--num=2000000 \
--YCSB_uniform_distribution=true \
--value_size=4096 \
--key_size=16 \
--statistics \
--max_bytes_for_level_multiplier=4 \
--max_background_compactions=16 \
--max_background_flushes=4 \
--statistics \
--cache_size=6442450944 \
--use_direct_io_for_flush_and_compaction \
-use_direct_reads \
--compression_type=none >> benchmarkslogycsb/Default100write.log

rm /mnt/* -rf

sleep 50

./db_bench_downcompact \
--benchmarks="ycsbfilldb,stats,ycsbwklda,stats" \
--db=/mnt/rocksdb_da \
--wal_dir=/mnt/rocksdb_wal \
--loadnum=50000000 \
--readwritepercent=20 \
--num=2000000 \
--YCSB_uniform_distribution=true \
--value_size=4096 \
--key_size=16 \
--statistics \
--max_bytes_for_level_multiplier=4 \
--max_background_compactions=16 \
--max_background_flushes=4 \
--statistics \
--cache_size=6442450944 \
--use_direct_io_for_flush_and_compaction \
-use_direct_reads \
--compression_type=none >> benchmarkslogycsb/Default80write.log

rm /mnt/* -rf

sleep 50


./db_bench_downcompact \
--benchmarks="ycsbfilldb,stats,ycsbwklda,stats" \
--db=/mnt/rocksdb_da \
--wal_dir=/mnt/rocksdb_wal \
--loadnum=50000000 \
--readwritepercent=80 \
--num=2000000 \
--YCSB_uniform_distribution=true \
--value_size=4096 \
--key_size=16 \
--statistics \
--max_bytes_for_level_multiplier=4 \
--max_background_compactions=16 \
--max_background_flushes=4 \
--statistics \
--cache_size=6442450944 \
--use_direct_io_for_flush_and_compaction \
-use_direct_reads \
--compression_type=none >> benchmarkslogycsb/Default20write.log

rm /mnt/* -rf

sleep 50


./db_bench_downcompact \
--benchmarks="ycsbfilldb,stats,ycsbwklda,stats" \
--db=/mnt/rocksdb_da \
--wal_dir=/mnt/rocksdb_wal \
--loadnum=50000000 \
--readwritepercent=100 \
--num=2000000 \
--YCSB_uniform_distribution=true \
--value_size=4096 \
--key_size=16 \
--statistics \
--max_bytes_for_level_multiplier=4 \
--max_background_compactions=16 \
--max_background_flushes=4 \
--statistics \
--cache_size=6442450944 \
--use_direct_io_for_flush_and_compaction \
-use_direct_reads \
--compression_type=none >> benchmarkslogycsb/Default0write.log