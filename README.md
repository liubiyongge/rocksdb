## RocksDB: A Persistent Key-Value Store for Flash and RAM Storage

```
./db_bench_default --benchmarks="ycsbfilldb,stats,ycsbwklda,stats" --db=/mnt/rocksdb_da --wal_dir=/mnt/rocksdb_wal --loadnum=50000000 --readwritepercent=0 --num=2000000 --YCSB_uniform_distribution=true --value_size=4096 --key_size=16 --target_file_size_base=8388608 --statistics --max_bytes_for_level_multiplier=4 --max_background_compactions=16 --max_background_flushes=4 --statistics --cache_size=6442450944 --use_direct_io_for_flush_and_compaction -use_direct_reads --compression_type=none --threads=40 >> benchmarkslogycsb/Default100write.log

```