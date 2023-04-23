#include "kraft/storage/raft_log_buffer.h"

#include <gtest/gtest.h>

using namespace kraft;

#define NUM 10

KRAFT_INLINE bool operator==(kraft::Entry const &x,
                             kraft::Entry const &y) noexcept
{
  return x.term() == y.term() && x.type() == y.type() &&
         x.index() == y.index() && x.data() == y.data();
}

RaftLogBuffer buffer;

TEST(raft_log_buffer, create)
{
  auto err = buffer.Init("1-logging.kraftlog", ReadOnlyFlag::OFF);
  ASSERT_EQ(buffer.GetStartIndex() == 1, true);
  buffer.SetEntriesNum(NUM);
  // buffer.SetStartIndex(1);
  ASSERT_EQ(buffer.GetCapacitry(), NUM);
  ASSERT_EQ(buffer.GetEndIndex(), 0);
  EXPECT_TRUE(!err);
  if (err) {
    printf("Failed to create buffer: %s\n", err.info()->GetMessage().c_str());
    perror("file error:");
    return;
  }

  if (buffer.TruncateAll(true)) {
    printf("Failed to empty the file\n");
    perror("Reason: ");
    return;
  }
}

TEST(raft_log_buffer, append)
{
  Entry entry;
  bool new_buffer;
  for (int i = 1; i <= NUM; ++i) {
    entry.set_index(i);
    entry.set_term(i);
    entry.set_type(kraft::ENTRY_NORMAL);
    entry.set_data(std::to_string(i));
    auto err = buffer.AppendEntry(entry, &new_buffer);
    if (err) {
      printf("Failed to append entry\nReason: %s\n",
             err.info()->GetMessage().c_str());
      break;
    }

    ASSERT_EQ(buffer[i] == entry, true)
        << "entry " << i << " is not required" << entry.DebugString() << "\n"
        << buffer[i].DebugString();
    ASSERT_EQ(buffer.has_logged_num(), i);

    printf("AE debug: %s\n", entry.DebugString().c_str());
  }
  ASSERT_EQ(buffer.GetEndIndex(), NUM);
  printf("log filename = %s\n", buffer.GetFilename().c_str());
}

TEST(raft_log_buffer, fetch_entries)
{
  auto err = buffer.FetchEntries();
  ASSERT_FALSE(err) << "Failed to fetch entries\nReason: "
                    << err.info()->GetMessage();

  auto start_index = buffer.GetStartIndex();
  auto end_index = buffer.GetEndIndex();

  EXPECT_EQ(start_index, 1);
  EXPECT_EQ(end_index, NUM);

  for (auto i = start_index; i <= end_index; ++i) {
    auto const &entry = buffer[i];
    EXPECT_EQ(entry.index(), i);
    EXPECT_EQ(entry.term(), i);
    EXPECT_EQ(entry.data(), std::to_string(i));
  }

  // Test iterator
  auto i = start_index;
  for (auto const &entry : buffer) {
    EXPECT_EQ(entry.index(), i);
    EXPECT_EQ(entry.term(), i);
    EXPECT_EQ(entry.data(), std::to_string(i));
    ++i;
  }
}

TEST(raft_log_buffer, truncate)
{
  ASSERT_FALSE(buffer.FetchEntries());
  ASSERT_FALSE(buffer.TruncateAfter(4, true));

  for (auto i = buffer.GetStartIndex(); i < buffer.GetEndIndex(); ++i) {
    auto const &entry = buffer[i];
    EXPECT_EQ(entry.index(), i);
    EXPECT_EQ(entry.term(), i);
    EXPECT_EQ(entry.data(), std::to_string(i));
  }
}
