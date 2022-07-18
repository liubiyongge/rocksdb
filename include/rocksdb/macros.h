#pragma once

//#define znskv_pri       //compaction-gc
//#define znskv_log      //日志
//#define GearDB       //合并窗口
#define TerarkDB       //迁移
//#define migrate 
//#define znskv_migrate  //Greedy
#define znskv_lifetime //寿命预测
#define znskv_allocate //数据布局
#define WITH_TERARKDB // open tearkdb garbage collection
//#define znskv_lifetime_pri //细粒度寿命
//#define multistream  //层级寿命
#define autopri //自适应调整文件选取 