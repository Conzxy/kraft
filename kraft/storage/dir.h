#ifndef _KRAFT_DIR_H__
#define _KRAFT_DIR_H__

#include <dirent.h>
#include <functional>
#include <string>
#include <sys/stat.h>
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

enum DirMode {
  DM_USER_READ = S_IRUSR,
  DM_USER_WRITE = S_IWUSR,
  DM_USER_EXEC = S_IXUSR,
  DM_GRP_READ = S_IRGRP,
  DM_GRP_WRITE = S_IWGRP,
  DM_GRP_EXEC = S_IXGRP,
  DM_OTH_READ = S_IROTH,
  DM_OTH_WRITE = S_IWOTH,
  DM_OTH_EXEC = S_IXOTH,
  DM_DEFAULT = DM_USER_READ | DM_USER_WRITE,
};

bool MakeDir(char const *dir, int mode = DM_DEFAULT);

KRAFT_INLINE bool MakeDir(std::string const &dir, int mode = DM_DEFAULT)
{
  return MakeDir(dir.data(), mode);
}

} // namespace kraft

#endif
