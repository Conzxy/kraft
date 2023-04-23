#include "kraft/storage/raft_log_buffer_pool.h"

#include <gtest/gtest.h>

using namespace kraft;
using namespace kerror;

RaftLogBufferPool pool;

std::vector<std::string> lognames;

TEST(raft_log_buffer_pool, main)
{
  int logfile_num = 6;
  const int entry_num = 100;
  int start_idx = 1;

  for (int i = 0; i < logfile_num; ++i) {
    RaftLogBuffer buffer;
    auto err = buffer.Init(RaftLogBuffer::MakeLoggingName(start_idx),
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
    auto p_buffer_or_err = pool.GetLogBuffer(lognames[i]);
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

TEST(raft_log_buffer_pool, victim)
{
  auto start_idx = 501;
  auto entry_num = 100;
  int i = 5;
  auto p_buffer_or_err = pool.GetLogBuffer(lognames[i]);
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
