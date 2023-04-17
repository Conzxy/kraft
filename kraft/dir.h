#ifndef _KRAFT_DIR_H__
#define _KRAFT_DIR_H__

#include <dirent.h>
#include <functional>
#include <string>
#include <sys/types.h>
#include <utility>

#include "kraft/macro.h"

namespace kraft {

using DirEntry = struct dirent;

class Dir {
 public:
  using ApplyDirEntryCallback = std::function<void(DirEntry const *ent)>;

  Dir();
  ~Dir() noexcept;
  Dir(Dir &&other) noexcept
    : dir_(other.dir_)
  {
    other.dir_ = nullptr;
  }

  Dir &operator=(Dir &&other) noexcept
  {
    // Not necessary
    // if (this == &other)
    std::swap(dir_, other.dir_);
    return *this;
  }

  KRAFT_DISABLE_COPY(Dir);

  bool Open(char const *name) noexcept;
  bool Open(std::string const &name) noexcept { return Open(name.c_str()); }

  bool ApplyDirEntry(ApplyDirEntryCallback apply_cb);
  bool ApplyDirEntries(ApplyDirEntryCallback apply_cb);

 private:
  DIR *dir_;
};

} // namespace kraft

#endif
