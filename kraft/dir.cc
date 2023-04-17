#include "dir.h"

#include <cassert>
#include <cstdlib>
#include <cstring>

#include "util/log.h"

using namespace kraft;

Dir::Dir()
  : dir_(nullptr)
{
}

Dir::~Dir() noexcept
{
  if (!dir_) return;

  if (-1 == closedir(dir_)) {
    perrorf("Failed to close directory");
  };
}

bool Dir::Open(char const *name) noexcept
{
  if (dir_) return false;

  KRAFT_ASSERT1(name);
  if ('~' == name[0]) {
    auto home_dir = getenv("HOME");
    if (home_dir) {
      std::string dir_path = home_dir;
      dir_path.append(name + 1, strlen(name) - 1);
      if (!(dir_ = opendir(dir_path.c_str()))) {
        perrorf("Failed to open dir: %s", dir_path.c_str());
        return false;
      }
      return true;
    }
  }

  // fallback
  if (!(dir_ = opendir(name))) {
    perrorf("Failed to open dir: %s", name);
    return false;
  }

  return true;
}

bool Dir::ApplyDirEntry(ApplyDirEntryCallback apply_cb)
{
  if (!dir_) return false;

  errno = 0;
  auto ent = readdir(dir_);
  if (!ent && errno != 0) {
    return false;
  }

  apply_cb(ent);
  return true;
}

bool Dir::ApplyDirEntries(ApplyDirEntryCallback apply_cb)
{
  if (!dir_) return false;

  errno = 0;

  auto ent = readdir(dir_);

  for (; ent; ent = readdir(dir_)) {
    apply_cb(ent);
  }

  if (errno != 0) {
    return false;
  }
  return true;
}
