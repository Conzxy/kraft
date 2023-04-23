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

#define KRAFT_FILE_NAME_LEN 256
#define KRAFT_UNUSED(x_)    (void)(x_)

#if defined(__GNUC__) || defined(__clang__)
#  define KRAFT_TLS __thread
#elif defined(_MSC_VER)
#  define KRAFT_TLS __declspec(thread)
#else
#  ifdef CXX_STANDARD_11
#    define KRAFT_TLS thread_local
#  else
#    define KRAFT_TLS
#  endif
#endif

#define DEFINE_BOOLEAN_ENUM_CLASS(enum_name__)                                 \
  enum class enum_name__ {                                                     \
    ON = 1,                                                                    \
    OFF = 0,                                                                   \
  };

#define KRAFT_ENTRY_LOG_EXT           ".kraftlog"
#define KRAFT_META_LOG_EXT            ".kraftmeta"
#define KRAFT_META_LOG_NAME           "kraftmeta"
#define KRAFT_LOGGING_LOG_NAME_SUFFIX "-logging.kraftlog"

#endif
