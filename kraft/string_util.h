#ifndef KRAFT_STRING_UTIL_H__
#define KRAFT_STRING_UTIL_H__

#include <cstring>

#include "kraft/type.h"

namespace kraft {

bool Raw2U64(void const *raw, size_t n, u64 *num) noexcept;

}

#endif
