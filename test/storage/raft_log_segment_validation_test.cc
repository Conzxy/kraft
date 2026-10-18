#include "kraft/storage/raft_log_segment.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>

#include "kraft/error_code.h"

using namespace kraft;

namespace fs = std::filesystem;

namespace {

struct Record {
  size_header_t size;
  std::string payload;
};

Record MakeRecord(Entry const &entry)
{
  return {static_cast<size_header_t>(entry.ByteSizeLong()),
          entry.SerializeAsString()};
}

Entry MakeEntry(u64 index, std::string data = {})
{
  Entry entry;
  entry.set_type(ENTRY_NORMAL);
  entry.set_term(1);
  entry.set_index(index);
  if (!data.empty()) entry.set_data(std::move(data));
  return entry;
}

void WriteRecords(std::string const &path,
                  std::initializer_list<Record> records)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << "failed to open " << path;

  for (auto const &record : records) {
    out.write(reinterpret_cast<char const *>(&record.size),
              sizeof record.size);
    out.write(record.payload.data(),
              static_cast<std::streamsize>(record.payload.size()));
  }
}

void WriteEmptyFile(std::string const &path)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << "failed to open " << path;
}

class RaftLogSegmentValidationTest : public ::testing::Test {
 protected:
  void SetUp() override
  {
    dir_ = fs::temp_directory_path() /
           ("kraft_segment_validation_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);
  }

  void TearDown() override
  {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  std::string Path(std::string const &name) const
  {
    return (dir_ / name).string();
  }

  fs::path dir_;
};

} // namespace

TEST_F(RaftLogSegmentValidationTest, rejects_zero_size_header)
{
  WriteRecords(Path("1-1.kraftlog"), {{0, ""}});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.Init(Path("1-1.kraftlog"), ReadOnlyFlag::ON));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  // kerror aborts in ~Error() unless the error was observed; operator bool()
  // only marks success as checked.
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_INVALID_LOG_ENTRY);
}

TEST_F(RaftLogSegmentValidationTest, rejects_oversized_size_header)
{
  // Must be rejected before entry_buf.resize() is called.
  WriteRecords(Path("1-1.kraftlog"),
               {{RaftLogSegment::kMaxEntrySize + 1, ""}});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.Init(Path("1-1.kraftlog"), ReadOnlyFlag::ON));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  // kerror aborts in ~Error() unless the error was observed; operator bool()
  // only marks success as checked.
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_ENTRY_TOO_LARGE);
}

TEST_F(RaftLogSegmentValidationTest, rejects_unparsable_payload)
{
  // 0xff is an invalid protobuf tag (field 31, wire type 7).
  WriteRecords(Path("1-1.kraftlog"), {{4, std::string(4, '\xff')}});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.Init(Path("1-1.kraftlog"), ReadOnlyFlag::ON));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  // kerror aborts in ~Error() unless the error was observed; operator bool()
  // only marks success as checked.
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_INVALID_LOG_ENTRY);
}

TEST_F(RaftLogSegmentValidationTest, rejects_missing_required_fields)
{
  // type = 0, but term/index are missing: ParseFromArray() succeeds while
  // IsInitialized() must fail.
  WriteRecords(Path("1-1.kraftlog"), {{2, std::string("\x08\x00", 2)}});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.Init(Path("1-1.kraftlog"), ReadOnlyFlag::ON));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  // kerror aborts in ~Error() unless the error was observed; operator bool()
  // only marks success as checked.
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_INVALID_LOG_ENTRY);
}

TEST_F(RaftLogSegmentValidationTest, rejects_non_continuous_index)
{
  WriteRecords(Path("1-2.kraftlog"),
               {MakeRecord(MakeEntry(1)), MakeRecord(MakeEntry(3))});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.Init(Path("1-2.kraftlog"), ReadOnlyFlag::ON));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  // kerror aborts in ~Error() unless the error was observed; operator bool()
  // only marks success as checked.
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_INVALID_LOG_ENTRY);
}

TEST_F(RaftLogSegmentValidationTest, loads_valid_entries)
{
  WriteRecords(Path("1-2.kraftlog"),
               {MakeRecord(MakeEntry(1, "one")), MakeRecord(MakeEntry(2, "two"))});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.Init(Path("1-2.kraftlog"), ReadOnlyFlag::ON));
  ASSERT_FALSE(segment.FetchEntries());

  ASSERT_EQ(segment.has_logged_num(), 2u);
  EXPECT_EQ(segment[1].index(), 1u);
  EXPECT_EQ(segment[1].data(), "one");
  EXPECT_EQ(segment[2].index(), 2u);
  EXPECT_EQ(segment[2].data(), "two");
}

TEST_F(RaftLogSegmentValidationTest, empty_logging_file_is_valid)
{
  WriteEmptyFile(Path("1-logging.kraftlog"));

  RaftLogSegment segment;
  ASSERT_FALSE(segment.Init(Path("1-logging.kraftlog"), ReadOnlyFlag::OFF));
  segment.SetEntriesNum(10);

  // Init() recovers a logging file; used to call entries_.front() on an empty
  // vector.
  EXPECT_EQ(segment.has_logged_num(), 0u);

  // Fetch methods are no-ops for a logging segment.
  EXPECT_FALSE(segment.FetchEntries());
  EXPECT_FALSE(segment.FetchEntriesBefore(2));
  EXPECT_EQ(segment.has_logged_num(), 0u);
}

TEST_F(RaftLogSegmentValidationTest, logging_entries_are_recovered_by_init)
{
  const auto path = Path("1-logging.kraftlog");
  {
    RaftLogSegment writer;
    ASSERT_FALSE(writer.Init(path, ReadOnlyFlag::OFF));
    writer.SetEntriesNum(10);

    Entry entry = MakeEntry(1, "one");
    bool new_segment = false;
    ASSERT_FALSE(writer.AppendEntry(entry, &new_segment));
    entry.set_index(2);
    entry.set_data("two");
    ASSERT_FALSE(writer.AppendEntry(entry, &new_segment));
  }

  // Reopen: Init() must rewind and recover the entries from disk.
  RaftLogSegment recovered;
  ASSERT_FALSE(recovered.Init(path, ReadOnlyFlag::OFF));
  recovered.SetEntriesNum(10);

  ASSERT_EQ(recovered.has_logged_num(), 2u);
  EXPECT_EQ(recovered[1].index(), 1u);
  EXPECT_EQ(recovered[1].data(), "one");
  EXPECT_EQ(recovered[2].index(), 2u);
  EXPECT_EQ(recovered[2].data(), "two");
}

TEST_F(RaftLogSegmentValidationTest, empty_sealed_file_is_rejected)
{
  WriteEmptyFile(Path("1-1.kraftlog"));

  RaftLogSegment segment;
  ASSERT_FALSE(segment.Init(Path("1-1.kraftlog"), ReadOnlyFlag::ON));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  // kerror aborts in ~Error() unless the error was observed; operator bool()
  // only marks success as checked.
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_SIZE_HEADER_INCOMPLTE);
}
