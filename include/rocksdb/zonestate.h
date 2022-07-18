#pragma once

#include <unistd.h>

#include <vector>
#include <unordered_map>

using namespace std;

extern unordered_map<uint64_t,float> zone_zoneunusepercent;
extern unordered_map<int,uint64_t> sst_zone;
extern unordered_map<int,int> sst_lifetime;
