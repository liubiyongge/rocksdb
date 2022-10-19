#pragma once
#include  <unordered_map>
#include "db/version_edit.h"


//f->fd.GetNumber()
std::unordered_map<uint64_t, InternalKey> smallestLables;
std::unordered_map<uint64_t, InternalKey> largestLables;
