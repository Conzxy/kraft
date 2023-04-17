#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cerrno>
#include <cstring>

void kraft::perrorf(char const *fmt, ...) noexcept
{
  char buf[4096];

  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof buf, fmt, args);

  auto saved_errno = errno;

  char error_buf[4096];
  const auto error_msg = strerror_r(saved_errno, error_buf, sizeof error_buf);
  fprintf(stderr, "%s - [errno: %d, msg: %s]\n", buf, saved_errno, error_msg);

  errno = 0;
  va_end(args);
}
