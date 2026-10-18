#include "kraft/storage/dir.h"

#include <gtest/gtest.h>

using namespace kraft;

TEST(Dir, open)
{
  Dir dir;
  EXPECT_TRUE(dir.Open("~"));

  Dir dir2;
  EXPECT_FALSE(dir2.Open("xxxxxxxx"));
}

static void test_dir(char const *dir_name)
{

  Dir dir;
  ASSERT_TRUE(dir.Open("~"));

  EXPECT_TRUE(dir.ApplyDirEntries([](DirEntry const &entry) {
    if (entry.IsRegularFile()) {
      printf("Regular file: %s\n", entry.name().c_str());
    } else if (entry.IsDirectory()) {
      printf("Dir: %s\n", entry.name().c_str());
    }
  }));
}

TEST(Dir, apply_entry)
{
  test_dir("~");
  test_dir("~/");
}

