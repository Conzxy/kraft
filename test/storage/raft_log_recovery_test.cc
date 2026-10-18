#include "kraft/storage/raft_log_manager.h"
#include "kraft/storage/raft_log_segment.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>

using namespace kraft;

namespace fs = std::filesystem;

namespace {

// Restore the working directory even when an ASSERT_* returns early.
class ScopedWorkingDir {
 public:
  explicit ScopedWorkingDir(fs::path const &dir) : old_dir_(fs::current_path())
  {
    fs::current_path(dir);
  }

  ~ScopedWorkingDir()
  {
    std::error_code ec;
    fs::current_path(old_dir_, ec);
  }

  ScopedWorkingDir(ScopedWorkingDir const &) = delete;
  ScopedWorkingDir &operator=(ScopedWorkingDir const &) = delete;

 private:
  fs::path old_dir_;
};

void AppendEntries(RaftLogManager *log, u64 from, u64 to)
{
  Entry entry;
  entry.set_type(ENTRY_NORMAL);
  for (u64 i = from; i <= to; ++i) {
    entry.set_term(i);
    entry.set_data(std::to_string(i));
    ASSERT_TRUE(log->AppendEntry(entry)) << "failed to append index " << i;
  }
}

void ExpectEntries(RaftLogManager *log, u64 from, u64 to)
{
  Entry const *entry = nullptr;
  for (u64 i = from; i <= to; ++i) {
    ASSERT_TRUE(log->GetEntry(i, &entry)) << "missing index " << i;
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->index(), i);
    EXPECT_EQ(entry->term(), i);
    EXPECT_EQ(entry->data(), std::to_string(i));
  }
}

class RaftLogRecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override
  {
    dir_ = fs::temp_directory_path() /
           ("kraft_log_recovery_" + std::to_string(::getpid()));
    log_dir_ = dir_ / "log";
    cwd_dir_ = dir_ / "cwd";

    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(log_dir_);
    fs::create_directories(cwd_dir_);
  }

  void TearDown() override
  {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  fs::path dir_;
  fs::path log_dir_;
  fs::path cwd_dir_;
};

} // namespace

// All files must be created in the log directory passed to Init(), not in the
// process working directory.
TEST_F(RaftLogRecoveryTest, uses_log_directory_not_cwd)
{
  ScopedWorkingDir cwd(cwd_dir_);

  {
    RaftLogManager log;
    ASSERT_FALSE(log.Init(log_dir_.string().c_str(), 10, 2));
    AppendEntries(&log, 1, 25);
  }

  EXPECT_TRUE(fs::exists(log_dir_ / "1-10.kraftlog"));
  EXPECT_TRUE(fs::exists(log_dir_ / "11-20.kraftlog"));
  EXPECT_TRUE(fs::exists(log_dir_ / "21-logging.kraftlog"));
  EXPECT_TRUE(fs::is_empty(cwd_dir_));

  RaftLogManager reopened;
  ASSERT_FALSE(reopened.Init(log_dir_.string().c_str(), 10, 2));
  ExpectEntries(&reopened, 1, 25);
  EXPECT_EQ(reopened.GetLastEntryMeta().index, 25u);
}

// If the process crashed after sealing the last segment but before creating the
// next logging file, recovery must create a segment starting at end + 1.
TEST_F(RaftLogRecoveryTest, recreates_logging_segment_at_last_end_plus_one)
{
  {
    RaftLogManager log;
    ASSERT_FALSE(log.Init(log_dir_.string().c_str(), 10, 1));
    AppendEntries(&log, 1, 20);
  }

  ASSERT_TRUE(fs::exists(log_dir_ / "11-20.kraftlog"));
  ASSERT_TRUE(fs::exists(log_dir_ / "21-logging.kraftlog"));
  fs::remove(log_dir_ / "21-logging.kraftlog");

  ScopedWorkingDir cwd(cwd_dir_);
  RaftLogManager log;
  ASSERT_FALSE(log.Init(log_dir_.string().c_str(), 10, 1));

  // The new segment starts at 21, not at the start (11) of the last sealed
  // segment, and it's created in the log directory.
  EXPECT_TRUE(fs::exists(log_dir_ / "21-logging.kraftlog"));
  EXPECT_FALSE(fs::exists(log_dir_ / "11-logging.kraftlog"));
  EXPECT_TRUE(fs::is_empty(cwd_dir_));

  AppendEntries(&log, 21, 25);
  ExpectEntries(&log, 1, 25);
  EXPECT_EQ(log.GetLastEntryMeta().index, 25u);
}

// The segment limit is the number of entries, not std::vector::capacity().
TEST_F(RaftLogRecoveryTest, capacity_is_max_entries_not_vector_capacity)
{
  RaftLogSegment segment;
  const auto path = (log_dir_ / "1-logging.kraftlog").string();
  ASSERT_FALSE(segment.Init(path.c_str(), ReadOnlyFlag::OFF));

  // Grow the vector first. SetEntriesNum() must still report the requested
  // entry limit afterwards, even though vector::reserve() never shrinks.
  segment.SetEntriesNum(64);
  segment.SetEntriesNum(10);
  EXPECT_EQ(segment.GetCapacitry(), 10u);

  Entry entry;
  entry.set_type(ENTRY_NORMAL);
  bool new_buffer = false;
  for (u64 i = 1; i <= 10; ++i) {
    entry.set_term(i);
    entry.set_data(std::to_string(i));
    ASSERT_FALSE(segment.AppendEntry(entry, &new_buffer));
  }

  // The 10th entry must seal the segment.
  EXPECT_TRUE(new_buffer);
  EXPECT_TRUE(fs::exists(log_dir_ / "1-10.kraftlog"));
  EXPECT_FALSE(fs::exists(log_dir_ / "1-logging.kraftlog"));
}
