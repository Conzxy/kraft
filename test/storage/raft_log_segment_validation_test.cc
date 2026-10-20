#include "kraft/storage/raft_log_segment.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "kraft/error_code.h"
#include "kraft/util/crc_codec_api.h"
#include "kraft/util/little_endian_codec_api.h"

using namespace kraft;

namespace fs = std::filesystem;

namespace {

struct Record {
  u32 size;
  u32 crc;
  std::string payload;
};

std::string MakeFileHeader(u32 magic = RaftLogSegment::kFileMagic,
                           u16 version = RaftLogSegment::kFileVersion,
                           u16 flags = 0)
{
  std::string header(RaftLogSegment::kFileHeaderSize, '\0');
  auto *p = reinterpret_cast<u8 *>(header.data());
  le::EncodeU32(p, magic);
  le::EncodeU16(p + sizeof(u32), version);
  le::EncodeU16(p + sizeof(u32) + sizeof(u16), flags);
  return header;
}

Record MakeRecord(Entry const &entry)
{
  auto payload = entry.SerializeAsString();
  const auto size = static_cast<u32>(payload.size());
  return {size, crc::ComputeRecord(size, payload.data()), std::move(payload)};
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

void WriteHeader(std::ofstream &out, std::string const &header)
{
  out.write(header.data(), static_cast<std::streamsize>(header.size()));
}

void WriteRecords(std::string const &path,
                  std::initializer_list<Record> records,
                  u16 version = RaftLogSegment::kFileVersion)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << "failed to open " << path;

  WriteHeader(out, MakeFileHeader(RaftLogSegment::kFileMagic, version));

  for (auto const &record : records) {
    u8 header[sizeof(u32) * 2];
    le::EncodeU32(header, record.size);
    le::EncodeU32(header + sizeof(u32), record.crc);
    out.write(reinterpret_cast<char const *>(header), sizeof header);
    out.write(record.payload.data(),
              static_cast<std::streamsize>(record.payload.size()));
  }
}

void WriteHeaderOnly(std::string const &path)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << "failed to open " << path;
  WriteHeader(out, MakeFileHeader());
}

void AppendBytes(std::string const &path, std::string const &bytes)
{
  std::ofstream out(path, std::ios::binary | std::ios::app);
  ASSERT_TRUE(out.is_open()) << "failed to open " << path;
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
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
  WriteRecords(Path("1-1.kraftlog"), {{0, 0, ""}});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-1.kraftlog")));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_INVALID_LOG_ENTRY);
}

TEST_F(RaftLogSegmentValidationTest, rejects_oversized_size_header)
{
  // Must be rejected before entry_buf.resize() is called.
  WriteRecords(Path("1-1.kraftlog"),
               {{RaftLogSegment::kMaxEntrySize + 1, 0, ""}});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-1.kraftlog")));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_ENTRY_TOO_LARGE);
}

TEST_F(RaftLogSegmentValidationTest, rejects_unparsable_payload)
{
  // 0xff is an invalid protobuf tag (field 31, wire type 7).
  const std::string payload(4, '\xff');
  WriteRecords(Path("1-1.kraftlog"),
               {{4, crc::ComputeRecord(4, payload.data()), payload}});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-1.kraftlog")));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_INVALID_LOG_ENTRY);
}

TEST_F(RaftLogSegmentValidationTest, rejects_missing_required_fields)
{
  const std::string payload("\x08\x00", 2);
  WriteRecords(Path("1-1.kraftlog"),
               {{2, crc::ComputeRecord(2, payload.data()), payload}});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-1.kraftlog")));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_INVALID_LOG_ENTRY);
}

TEST_F(RaftLogSegmentValidationTest, rejects_non_continuous_index)
{
  WriteRecords(Path("1-2.kraftlog"),
               {MakeRecord(MakeEntry(1)), MakeRecord(MakeEntry(3))});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-2.kraftlog")));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_INVALID_LOG_ENTRY);
}

TEST_F(RaftLogSegmentValidationTest, rejects_corrupt_payload)
{
  auto record = MakeRecord(MakeEntry(1, "one"));
  record.payload.back() ^= 0x01;

  WriteRecords(Path("1-1.kraftlog"), {record});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-1.kraftlog")));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_LOG_CHECKSUM_MISMATCH);
}

TEST_F(RaftLogSegmentValidationTest, rejects_corrupt_crc)
{
  auto record = MakeRecord(MakeEntry(1, "one"));
  record.crc ^= 0xFFFFFFFF;

  WriteRecords(Path("1-1.kraftlog"), {record});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-1.kraftlog")));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_LOG_CHECKSUM_MISMATCH);
}

TEST_F(RaftLogSegmentValidationTest, rejects_unsupported_version)
{
  WriteRecords(Path("1-1.kraftlog"), {}, /*version=*/2);

  RaftLogSegment segment;
  errcode = E_OK;
  auto err = segment.InitAsSealed(Path("1-1.kraftlog"));
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_INVALID_LOG_VERSION);
}

TEST_F(RaftLogSegmentValidationTest, loads_valid_entries)
{
  WriteRecords(Path("1-2.kraftlog"),
               {MakeRecord(MakeEntry(1, "one")), MakeRecord(MakeEntry(2, "two"))});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-2.kraftlog")));
  ASSERT_FALSE(segment.FetchEntries());

  ASSERT_EQ(segment.has_logged_num(), 2u);
  EXPECT_EQ(segment[1].index(), 1u);
  EXPECT_EQ(segment[1].data(), "one");
  EXPECT_EQ(segment[2].index(), 2u);
  EXPECT_EQ(segment[2].data(), "two");
}

TEST_F(RaftLogSegmentValidationTest, empty_logging_file_is_valid)
{
  WriteHeaderOnly(Path("1-logging.kraftlog"));

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsLogging(Path("1-logging.kraftlog")));
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
    ASSERT_FALSE(writer.InitAsLogging(path));
    writer.SetEntriesNum(10);

    Entry entry = MakeEntry(1, "one");
    bool new_segment = false;
    ASSERT_FALSE(writer.AppendEntry(entry, &new_segment));
    entry.set_index(2);
    entry.set_data("two");
    ASSERT_FALSE(writer.AppendEntry(entry, &new_segment));
  }

  // Reopen: Init() must recover the entries from disk.
  RaftLogSegment recovered;
  ASSERT_FALSE(recovered.InitAsLogging(path));
  recovered.SetEntriesNum(10);

  ASSERT_EQ(recovered.has_logged_num(), 2u);
  EXPECT_EQ(recovered[1].index(), 1u);
  EXPECT_EQ(recovered[1].data(), "one");
  EXPECT_EQ(recovered[2].index(), 2u);
  EXPECT_EQ(recovered[2].data(), "two");
}

TEST_F(RaftLogSegmentValidationTest, empty_sealed_file_is_rejected)
{
  WriteHeaderOnly(Path("1-1.kraftlog"));

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-1.kraftlog")));

  errcode = E_OK;
  auto err = segment.FetchEntries();
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_SIZE_HEADER_INCOMPLTE);
}

TEST_F(RaftLogSegmentValidationTest, truncates_torn_record_header)
{
  const auto path = Path("1-logging.kraftlog");
  size_t valid_size = 0;
  {
    RaftLogSegment writer;
    ASSERT_FALSE(writer.InitAsLogging(path));
    writer.SetEntriesNum(10);

    Entry entry = MakeEntry(1, "one");
    bool new_segment = false;
    ASSERT_FALSE(writer.AppendEntry(entry, &new_segment));
    entry.set_index(2);
    entry.set_data("two");
    ASSERT_FALSE(writer.AppendEntry(entry, &new_segment));
  }

  valid_size = fs::file_size(path);
  AppendBytes(path, std::string(3, '\x7f')); // partial record header

  RaftLogSegment recovered;
  ASSERT_FALSE(recovered.InitAsLogging(path));
  recovered.SetEntriesNum(10);

  EXPECT_EQ(recovered.has_logged_num(), 2u);
  EXPECT_EQ(fs::file_size(path), valid_size);

  // The recovered file must still be appendable.
  Entry entry = MakeEntry(3, "three");
  bool new_segment = false;
  ASSERT_FALSE(recovered.AppendEntry(entry, &new_segment));
}

TEST_F(RaftLogSegmentValidationTest, truncates_torn_payload)
{
  const auto path = Path("1-logging.kraftlog");
  size_t valid_size = 0;
  {
    RaftLogSegment writer;
    ASSERT_FALSE(writer.InitAsLogging(path));
    writer.SetEntriesNum(10);

    Entry entry = MakeEntry(1, "one");
    bool new_segment = false;
    ASSERT_FALSE(writer.AppendEntry(entry, &new_segment));
  }

  valid_size = fs::file_size(path);

  // A complete record header announcing 100 payload bytes, followed by only
  // 10 bytes.
  std::string torn(sizeof(u32) * 2 + 10, '\0');
  auto *p = reinterpret_cast<u8 *>(torn.data());
  le::EncodeU32(p, 100);
  le::EncodeU32(p + sizeof(u32), 0);
  AppendBytes(path, torn);

  RaftLogSegment recovered;
  ASSERT_FALSE(recovered.InitAsLogging(path));
  recovered.SetEntriesNum(10);

  EXPECT_EQ(recovered.has_logged_num(), 1u);
  EXPECT_EQ(fs::file_size(path), valid_size);
}

TEST_F(RaftLogSegmentValidationTest, does_not_truncate_checksum_mismatch)
{
  const auto path = Path("1-logging.kraftlog");
  {
    RaftLogSegment writer;
    ASSERT_FALSE(writer.InitAsLogging(path));
    writer.SetEntriesNum(10);

    Entry entry = MakeEntry(1, "one");
    bool new_segment = false;
    ASSERT_FALSE(writer.AppendEntry(entry, &new_segment));
    entry.set_index(2);
    entry.set_data("two");
    ASSERT_FALSE(writer.AppendEntry(entry, &new_segment));
  }

  // Flip a payload byte of the last record.
  const auto size_before = fs::file_size(path);
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekg(-1, std::ios::end);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= 0x01;
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
  }

  RaftLogSegment recovered;
  errcode = E_OK;
  auto err = recovered.InitAsLogging(path);
  EXPECT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_LOG_CHECKSUM_MISMATCH);
  EXPECT_EQ(fs::file_size(path), size_before);
}

TEST_F(RaftLogSegmentValidationTest, sealed_segment_rejects_writes)
{
  WriteRecords(Path("1-2.kraftlog"),
               {MakeRecord(MakeEntry(1)), MakeRecord(MakeEntry(2))});

  RaftLogSegment segment;
  ASSERT_FALSE(segment.InitAsSealed(Path("1-2.kraftlog")));
  EXPECT_TRUE(segment.IsSealed());

  Entry entry = MakeEntry(3);
  bool new_segment = false;
  errcode = E_OK;
  auto append_err = segment.AppendEntry(entry, &new_segment);
  ASSERT_TRUE(append_err);
  append_err.IgnoreCheck();
  EXPECT_EQ(errcode, E_SEGMENT_READONLY);

  errcode = E_OK;
  auto truncate_err = segment.TruncateAfter(1, true);
  ASSERT_TRUE(truncate_err);
  truncate_err.IgnoreCheck();
  EXPECT_EQ(errcode, E_SEGMENT_READONLY);

  segment.SwitchToLogging();
  EXPECT_TRUE(segment.IsLogging());
  EXPECT_FALSE(segment.IsSealed());
}
