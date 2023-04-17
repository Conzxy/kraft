#ifndef _KRAFT_UTIL_H__
#define _KRAFT_UTIL_H__

#include "kraft.pb.h"
#include "kraft/macro.h"
#include "kraft/type.h"

namespace kraft {

KRAFT_INLINE EntryMeta Entry2Meta(Entry const &ety) noexcept
{
  EntryMeta meta;
  meta.index = ety.index();
  meta.term = ety.term();
  return meta;
}

} // namespace kraft

#endif
