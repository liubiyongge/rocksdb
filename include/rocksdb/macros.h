#pragma once

// #define znskv_pri       //compaction-gc
// #define znskv_log      //日志
// #define GearDB       //合并窗口
// #define TerarkDB       //迁移
// //#define migrate_speed     //分配新zone用于迁移 默认迁移不适用新的zone implement in future
// #define znskv_migrate  //Greedy
// #define znskv_lifetime //寿命预测

// #define znskv_allocate //数据布局
// //#define multistream  //层级寿命
// #define autopri //自适应调整文件选取
#define LOGFILE "/tmp/zonesstinzone.txt" 
#define SPDLOGFILE "/tmp/spdlogfile.txt"
#define SSTLIFETIMESTAT