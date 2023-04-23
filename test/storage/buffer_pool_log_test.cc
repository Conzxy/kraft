#include "kraft/storage/buffer_pool_log.h"

#include <gtest/gtest.h>

using namespace kraft;

int N = 100;
int start_idx = 1;
int end_idx = N;

BufferPoolLog log_eg;

TEST(buffer_pool_log, init)
{
  system("rm -rf *.kraftlog");
  auto err = log_eg.Init(".", 10);
  if (err) {
    fprintf(stderr, "Failed to init log engine\n");
    PErrorSys(err);
    abort();
  }
}

TEST(buffer_pool_log, append)
{
  Entry entry;
  entry.set_type(kraft::ENTRY_NORMAL);

  for (int i = start_idx; i <= end_idx; ++i) {
    entry.set_term(i);
    entry.set_data(std::to_string(i));
    log_eg.AppendEntry(entry);
  }
}

TEST(buffer_pool_log, update_state)
{
  PersistentState state;
  state.set_term(100);
  state.set_id(1);
  state.set_voted_for(124);

  ASSERT_TRUE(log_eg.UpdateState(state));

  PersistentState state2;
  log_eg.FetchState(&state2);
  ASSERT_EQ(state.term(), state2.term());
  ASSERT_EQ(state.id(), state2.id());
  ASSERT_EQ(state.voted_for(), state2.voted_for());
}

TEST(buffer_pool_log, get_last_entry_meta)
{
  auto meta = log_eg.GetLastEntryMeta();

  ASSERT_EQ(meta.term, end_idx);
  ASSERT_EQ(meta.index, end_idx);
}

TEST(buffer_pool_log, get_entry)
{
  log_eg.DebugPrint();
  Entry const *p_entry = nullptr;
  for (int i = start_idx; i <= end_idx; ++i) {
    ASSERT_TRUE(log_eg.GetEntry(i, &p_entry));
    ASSERT_EQ(p_entry->term(), i);
    ASSERT_EQ(p_entry->index(), i);
    ASSERT_EQ(p_entry->type(), ENTRY_NORMAL);
    ASSERT_EQ(p_entry->data(), std::to_string(i));
  }
}

TEST(buffer_pool_log, get_entry_meta)
{
  for (int i = start_idx; i <= end_idx; ++i) {
    auto term = log_eg.GetEntryMeta(i);
    ASSERT_EQ(term, i);
  }
}

TEST(buffer_pool_log, truncate_after)
{
  ASSERT_EQ(log_eg.TruncateAfter(45), true);
  auto last_entry_meta = log_eg.GetLastEntryMeta();
  ASSERT_EQ(last_entry_meta.index, 45);
  ASSERT_EQ(last_entry_meta.term, 45);
}

BufferPoolLog log_eg2;

TEST(buffer_pool_log2, init)
{
  end_idx = 45;
  auto err = log_eg2.Init(".", 20);
  ASSERT_FALSE(err) << "Failed to init log engine" << err.info()->GetMessage();
}

TEST(buffer_pool_log2, get_last_entry_meta_old)
{
  auto meta = log_eg2.GetLastEntryMeta();

  ASSERT_EQ(meta.term, end_idx);
  ASSERT_EQ(meta.index, end_idx);
}

TEST(buffer_pool_log2, get_entry_meta_old)
{
  for (int i = start_idx; i <= end_idx; ++i) {
    auto term = log_eg2.GetEntryMeta(i);
    ASSERT_EQ(term, i);
  }
}

TEST(buffer_pool_log2, get_entry_old)
{
  Entry const *p_entry = nullptr;
  for (int i = start_idx; i <= end_idx; ++i) {
    ASSERT_TRUE(log_eg2.GetEntry(i, &p_entry));
    ASSERT_EQ(p_entry->term(), i);
    ASSERT_EQ(p_entry->index(), i);
    ASSERT_EQ(p_entry->type(), ENTRY_NORMAL);
    ASSERT_EQ(p_entry->data(), std::to_string(i));
  }
}

TEST(buffer_pool_log2, append)
{
  start_idx = end_idx + 1;
  end_idx = end_idx + N;

  Entry entry;
  entry.set_type(kraft::ENTRY_NORMAL);

  for (int i = start_idx; i <= end_idx; ++i) {
    entry.set_term(i);
    entry.set_data(std::to_string(i));
    log_eg2.AppendEntry(entry);
  }
}

TEST(buffer_pool_log2, get_last_entry_meta)
{
  auto meta = log_eg2.GetLastEntryMeta();

  ASSERT_EQ(meta.term, end_idx);
  ASSERT_EQ(meta.index, end_idx);
}

TEST(buffer_pool_log2, get_entry_meta)
{
  for (int i = 1; i <= end_idx; ++i) {
    auto term = log_eg2.GetEntryMeta(i);
    if (term == INVALID_INDEX) {
      abort();
    }
    ASSERT_EQ(term, i);
  }
}

TEST(buffer_pool_log2, get_entry)
{
  Entry const *p_entry = nullptr;
  for (int i = 1; i <= end_idx; ++i) {
    ASSERT_TRUE(log_eg2.GetEntry(i, &p_entry));
    ASSERT_EQ(p_entry->term(), i);
    ASSERT_EQ(p_entry->index(), i);
    ASSERT_EQ(p_entry->type(), ENTRY_NORMAL);
    ASSERT_EQ(p_entry->data(), std::to_string(i));
  }
}
