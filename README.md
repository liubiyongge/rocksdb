## RocksDB: A Persistent Key-Value Store for Flash and RAM Storage

[![CircleCI Status](https://circleci.com/gh/facebook/rocksdb.svg?style=svg)](https://circleci.com/gh/facebook/rocksdb)
[![TravisCI Status](https://api.travis-ci.com/facebook/rocksdb.svg?branch=main)](https://travis-ci.com/github/facebook/rocksdb)
[![Appveyor Build status](https://ci.appveyor.com/api/projects/status/fbgfu0so3afcno78/branch/main?svg=true)](https://ci.appveyor.com/project/Facebook/rocksdb/branch/main)
[![PPC64le Build Status](http://140-211-168-68-openstack.osuosl.org:8080/buildStatus/icon?job=rocksdb&style=plastic)](http://140-211-168-68-openstack.osuosl.org:8080/job/rocksdb)

RocksDB is developed and maintained by Facebook Database Engineering Team.
It is built on earlier work on [LevelDB](https://github.com/google/leveldb) by Sanjay Ghemawat (sanjay@google.com)
and Jeff Dean (jeff@google.com)

This code is a library that forms the core building block for a fast
key-value server, especially suited for storing data on flash drives.
It has a Log-Structured-Merge-Database (LSM) design with flexible tradeoffs
between Write-Amplification-Factor (WAF), Read-Amplification-Factor (RAF)
and Space-Amplification-Factor (SAF). It has multi-threaded compactions,
making it especially suitable for storing multiple terabytes of data in a
single database.

Start with example usage here: https://github.com/facebook/rocksdb/tree/main/examples

See the [github wiki](https://github.com/facebook/rocksdb/wiki) for more explanation.

The public interface is in `include/`.  Callers should not include or
rely on the details of any other header files in this package.  Those
internal APIs may be changed without warning.

Questions and discussions are welcome on the [RocksDB Developers Public](https://www.facebook.com/groups/rocksdb.dev/) Facebook group and [email list](https://groups.google.com/g/rocksdb) on Google Groups.

## Complile and run
1. compile rocksdb

```
sudo DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make -j48 db_bench install
# must install
# do not use cmake, it is too old. build by make.
```
2. compile zenfs util

```
cd plugin/zenfs/util
make
```

3. dbbench prepare
```
sudo su
echo deadline > /sys/class/block/nvme0n1/queue/scheduler
# mkdir /home/lby/zfsfile/
/home/lby/rocksdb/plugin/zenfs/util/zenfs mkfs --zbd=nvme0n1 --aux_path=/home/lby/zfsfile/ --force
```
4. run dbbench
```
/home/lby/rocksdb/db_bench --fs_uri=zenfs://dev:nvme0n1 --benchmarks="fillrandom,stats" \
--use_direct_reads --key_size=16 --value_size=800 \
       --target_file_size_base=2147483648 \
       --use_direct_io_for_flush_and_compaction \
       --max_bytes_for_level_multiplier=4 --write_buffer_size=2147483648 \
       --target_file_size_multiplier=1 --num=10000000 --threads=2 \
       --max_background_jobs=4 \
       -compression_type=None --statistics >> dblog
```
### config file
- include/rocksdb/macros.h
- plugin/zenfs/fs/macros.h

## License

RocksDB is dual-licensed under both the GPLv2 (found in the COPYING file in the root directory) and Apache 2.0 License (found in the LICENSE.Apache file in the root directory).  You may select, at your option, one of the above-listed licenses.
