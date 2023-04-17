#include "kraft/dir.h"

#include <gtest/gtest.h>

using namespace kraft;

TEST(Dir, open)
{
  Dir dir;
  EXPECT_TRUE(dir.Open("~"));

  Dir dir2;
  EXPECT_FALSE(dir2.Open("xxxxxxxx"));
}

TEST(Dir, apply_entry)
{
  Dir dir;
  ASSERT_TRUE(dir.Open("~"));

  EXPECT_TRUE(dir.ApplyDirEntries([](DirEntry const *entry) {
    if (DT_REG == entry->d_type) {
      printf("Regular file: %s\n", entry->d_name);
    } else if (DT_DIR == entry->d_type) {
      printf("Dir: %s\n", entry->d_name);
    }
  }));
}
