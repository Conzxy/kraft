#include "kraft/string_util.h"

#include "kraft/macro.h"
#include <cassert>
#include <cstdlib>

bool kraft::Raw2U64(void const *raw, size_t n, u64 *num) noexcept
{
  KRAFT_ASSERT(n <= 64, "The size of raw buffer must be <= 64");
  KRAFT_ASSERT1(num);
  char buf[64];

  memcpy(buf, raw, n);
  buf[n] = 0;

  char *end_ptr = nullptr;

  *num = strtoull(buf, &end_ptr, 10);

  if (end_ptr && 0 == *num) {
    return false;
  }
  return true;
}
