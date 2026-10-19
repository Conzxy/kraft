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

/**
 * A wrapper of `struct dirent`.
 *
 * The entry returned by readdir() is only valid until the next readdir()
 * call, so the wrapper owns a copy of the fields we need.
 */
class DirEntry {
 public:
  DirEntry() noexcept = default;

  explicit DirEntry(struct dirent const &entry)
    : name_(entry.d_name)
    , type_(entry.d_type)
    , ino_(entry.d_ino)
  {
  }

  KRAFT_INLINE std::string const &name() const noexcept { return name_; }
  KRAFT_INLINE unsigned char type() const noexcept { return type_; }
  KRAFT_INLINE ino_t ino() const noexcept { return ino_; }

  KRAFT_INLINE bool IsRegularFile() const noexcept
  {
    return DT_REG == type_;
  }
  KRAFT_INLINE bool IsDirectory() const noexcept { return DT_DIR == type_; }
  KRAFT_INLINE bool IsSymlink() const noexcept { return DT_LNK == type_; }

 private:
  std::string name_;
  unsigned char type_ = DT_UNKNOWN;
  ino_t ino_ = 0;
};

class Dir {
 public:
  using ApplyDirEntryCallback = std::function<void(DirEntry const &ent)>;

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

  /**
   * \brief Flush the directory's metadata.
   *
   * Call this after rename()/unlink() when the directory entry change must be
   * durable.
   */
  bool Fsync() noexcept;

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
