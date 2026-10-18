#include "kraft/storage/raft_log_segment_pool.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>

using namespace kraft;
using namespace kerror;

namespace fs = std::filesystem;

RaftLogSegmentPool pool;

std::vector<std::string> lognames;

TEST(raft_log_segment_pool, main)
{
  int logfile_num = 6;
  const int entry_num = 100;
  int start_idx = 1;

  for (int i = 0; i < logfile_num; ++i) {
    RaftLogSegment buffer;
    auto err = buffer.Init(RaftLogSegment::MakeLoggingName(start_idx),
                           ReadOnlyFlag::OFF);
    buffer.SetEntriesNum(entry_num);
    Entry entry;
    bool new_buffer;
    for (int j = 0; j < entry_num; ++j) {
      entry.set_term(i);
      entry.set_index(start_idx + j);
      entry.set_data(std::to_string(entry.index()));
      entry.set_type(kraft::ENTRY_NORMAL);
      auto err = buffer.AppendEntry(entry, &new_buffer);
      if (err) {
        printf("Failed to append entry [%d]\n", start_idx + j);
        printf("Reason: %s\n", err.info()->GetMessage().c_str());
        abort();
      }
    }

    start_idx += entry_num;
    lognames.emplace_back(buffer.GetFilename());
  }

  pool.Init(5);
  start_idx = 1;
  for (int i = 0; i < 5; ++i) {
    auto p_buffer_or_err = pool.GetLogSegment(lognames[i]);
    printf("logname: %s\n", lognames[i].c_str());
    if (p_buffer_or_err) {
      fprintf(stderr, "Failed to get the buffer [%d]\n", i);
      printf("Reason: %s\n", p_buffer_or_err.info()->GetMessage().c_str());
      abort();
    }

    auto &buffer = **p_buffer_or_err;
    auto err = buffer.FetchEntries();
    if (err) {
      fprintf(stderr, "Failed to fetch entries from buffer [%d]\n", i);
      PError(err);
      abort();
    }

    ASSERT_EQ(buffer.GetStartIndex(), start_idx);
    for (int j = 0; j < entry_num; ++j) {
      ASSERT_EQ(buffer[start_idx + j].index() == (u64)start_idx + j, true);
    }

    start_idx += entry_num;
  }
}

TEST(raft_log_segment_pool, victim)
{
  auto start_idx = 501;
  auto entry_num = 100;
  int i = 5;
  auto p_buffer_or_err = pool.GetLogSegment(lognames[i]);
  if (p_buffer_or_err) {
    fprintf(stderr, "Failed to get the buffer [%d]\n", i);
    printf("Reason: %s\n", p_buffer_or_err.info()->GetMessage().c_str());
    abort();
  }

  auto &buffer = **p_buffer_or_err;
  auto err = buffer.FetchEntries();
  if (err) {
    fprintf(stderr, "Failed to fetch_entries from buffer [%d]\n", i);
    PError(err);
    abort();
  }

  ASSERT_EQ(buffer.GetStartIndex(), start_idx);
  for (int j = 0; j < entry_num; ++j) {
    ASSERT_EQ(buffer[start_idx + j].index() == (u64)start_idx + j, true);
  }
}

namespace {

class RaftLogSegmentPoolCacheTest : public ::testing::Test {
 protected:
  void SetUp() override
  {
    dir_ = fs::temp_directory_path() /
           ("kraft_segment_pool_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);
  }

  void TearDown() override
  {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  // Append @p count entries starting at @p start and let the segment seal
  // itself. Returns the sealed path.
  std::string MakeSealedSegment(u64 start, size_t count)
  {
    RaftLogSegment segment;
    const auto logging_path =
        (dir_ / RaftLogSegment::MakeLoggingName(start)).string();

    EXPECT_FALSE(segment.Init(logging_path, ReadOnlyFlag::OFF));
    segment.SetEntriesNum(count);

    Entry entry;
    entry.set_type(ENTRY_NORMAL);
    bool new_segment = false;
    for (u64 i = start; i < start + count; ++i) {
      entry.set_term(1);
      entry.set_index(i);
      entry.set_data(std::to_string(i));
      EXPECT_FALSE(segment.AppendEntry(entry, &new_segment));
    }
    EXPECT_TRUE(new_segment);
    return segment.GetFilename();
  }

  fs::path dir_;
};

} // namespace

// A cache lookup must not depend on the address of the std::string used as the
// key: two live copies of the same path must hit the same slot.
TEST_F(RaftLogSegmentPoolCacheTest, path_identity_is_value_based)
{
  const auto path = MakeSealedSegment(1, 10);

  std::string path_copy1 = path;
  std::string path_copy2 = path;

  RaftLogSegmentPool pool;
  pool.Init(2);

  auto first = pool.GetLogSegment(path_copy1);
  ASSERT_FALSE(first);
  auto second = pool.GetLogSegment(path_copy2);
  ASSERT_FALSE(second);

  EXPECT_EQ(*first, *second);
  EXPECT_EQ((*second)->GetStartIndex(), 1u);
}

// Removing a segment frees its slot; loading another segment into that slot
// must not touch any stale bookkeeping (this used to be a use-after-free).
TEST_F(RaftLogSegmentPoolCacheTest, remove_then_reuse_slot)
{
  const auto path_a = MakeSealedSegment(1, 10);
  const auto path_b = MakeSealedSegment(11, 10);

  RaftLogSegmentPool pool;
  pool.Init(1);

  auto loaded_a = pool.GetLogSegment(path_a);
  ASSERT_FALSE(loaded_a);
  ASSERT_EQ((*loaded_a)->GetStartIndex(), 1u);

  ASSERT_FALSE(pool.RemoveSegment(path_a));
  ASSERT_FALSE(pool.RemoveSegment(path_a)); // idempotent

  auto loaded_b = pool.GetLogSegment(path_b);
  ASSERT_FALSE(loaded_b);
  EXPECT_EQ((*loaded_b)->GetStartIndex(), 11u);
  EXPECT_EQ((*loaded_b)->GetFilename(), path_b);

  // The removed path must be loadable again.
  auto reloaded_a = pool.GetLogSegment(path_a);
  ASSERT_FALSE(reloaded_a);
  EXPECT_EQ((*reloaded_a)->GetStartIndex(), 1u);
  EXPECT_EQ((*reloaded_a)->GetFilename(), path_a);
}

// Eviction must only drop the cache entry: the segment must be reloadable.
TEST_F(RaftLogSegmentPoolCacheTest, reloads_after_eviction)
{
  const auto path_a = MakeSealedSegment(1, 10);
  const auto path_b = MakeSealedSegment(11, 10);

  RaftLogSegmentPool pool;
  pool.Init(1);

  auto a = pool.GetLogSegment(path_a);
  ASSERT_FALSE(a);
  ASSERT_FALSE((*a)->FetchEntries());

  // Evict A.
  auto b = pool.GetLogSegment(path_b);
  ASSERT_FALSE(b);
  ASSERT_EQ((*b)->GetStartIndex(), 11u);

  auto a_again = pool.GetLogSegment(path_a);
  ASSERT_FALSE(a_again);
  ASSERT_FALSE((*a_again)->FetchEntries());
  ASSERT_EQ((*a_again)->GetStartIndex(), 1u);
  ASSERT_EQ((*a_again)->GetEndIndex(), 10u);

  for (u64 i = 1; i <= 10; ++i) {
    EXPECT_EQ((*a_again)->operator[](i).index(), i);
  }
}
