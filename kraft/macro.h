// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_MACRO_H__
#define _KRAFT_MACRO_H__

#if defined(__GNUC__) || defined(__clang__)
#  define KRAFT_ALWAYS_INLINE __attribute__((always_inline))
#elif defined(_MSC_VER) /* && !defined(__clang__) */
#  define KRAFT_ALWAYS_INLINE __forceinline
#else
#  define KRAFT_ALWAYS_INLINE
#endif // !defined(__GNUC__) || defined(__clang__)

#define KRAFT_INLINE inline KRAFT_ALWAYS_INLINE

#define KRAFT_ASSERT(cond_, msg_) assert((cond_) && (msg_))

#define KRAFT_ASSERT1(cond_) assert(cond_)

#define KRAFT_MIN(x, y) ((x) < (y) ? (x) : (y))
#define KRAFT_MAX(x, y) ((x) < (y) ? (y) : (x))

#define KRAFT_DISABLE_COPY(class_)                                             \
  class_ &operator=(class_ const &) = delete;                                  \
  class_(class_ const &) = delete

#endif
