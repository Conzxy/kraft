#include "buffer_pool_log.h"

using namespace kraft;

struct BufferPoolLog::BufferPoolLogImpl {

  KRAFT_INLINE static auto MakePath(BufferPoolLog *me, char const *name,
                                    size_t n) -> std::string
  {
    std::string new_path = me->dir_path_;
    if (new_path.back() != '/') {
      new_path += '/';
    }
    new_path.append(name, n);
    return new_path;
  }
};
