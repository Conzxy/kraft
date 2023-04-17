// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_TYPE_H__
#define _KRAFT_TYPE_H__

#include <stdint.h>
#include <unordered_map>

namespace kraft {

template <typename K, typename V, typename Hash = std::hash<K>,
          typename Pred = std::equal_to<K>,
          typename Alloc = std::allocator<std::pair<K const, V>>>
using HashMap = std::unordered_map<K, V, Hash, Pred, Alloc>;

using u64 = uint64_t;
using u8 = uint8_t;

// The metadata describe the log entry
//
// NOTE: We don't care the entry type(confchange or normal operation)
struct EntryMeta {
  u64 term = -1;
  u64 index = -1;
};

} // namespace kraft

#endif
