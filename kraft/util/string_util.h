// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef KRAFT_STRING_UTIL_H__
#define KRAFT_STRING_UTIL_H__

#include <cstring>
#include <string>

#include "kraft/type.h"

namespace kraft {

bool Raw2U64(void const *raw, size_t n, u64 *num) noexcept;

std::string GetFilenameWithoutDir(std::string const &path);

} // namespace kraft

#endif
