#include "kraft/storage/raft_log_meta_record.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "kraft/error_code.h"
#include "kraft/util/little_endian_codec_api.h"

using namespace kraft;

namespace fs = std::filesystem;

namespace {

PersistentState MakeState(u64 term, u64 voted_for, u64 id)
{
  PersistentState state;
  state.set_term(term);
  state.set_voted_for(voted_for);
  state.set_id(id);
  return state;
}

std::string ReadAll(fs::path const &path)
{
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open()) << "failed to open " << path;
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void WriteAll(fs::path const &path, std::string const &data)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << "failed to open " << path;
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void AppendBytes(fs::path const &path, std::string const &data)
{
  std::ofstream out(path, std::ios::binary | std::ios::app);
  ASSERT_TRUE(out.is_open()) << "failed to open " << path;
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

class RaftLogMetaRecordTest : public ::testing::Test {
 protected:
  void SetUp() override
  {
    dir_ = fs::temp_directory_path() /
           ("kraft_meta_record_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);
  }

  void TearDown() override
  {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  fs::path Path() const { return dir_ / "kraftmeta"; }

  fs::path dir_;
};

void ExpectLoadError(fs::path const &path, int expected_errcode)
{
  errcode = E_OK;
  auto loaded = RaftLogMetaRecord::Load(path.string());
  ASSERT_TRUE(loaded);
  loaded.error().IgnoreCheck();
  EXPECT_EQ(errcode, expected_errcode);
}

} // namespace

TEST_F(RaftLogMetaRecordTest, stores_and_loads)
{
  const auto state = MakeState(7, 3, 42);
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), state));

  auto loaded = RaftLogMetaRecord::Load(Path().string());
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded->term(), 7u);
  EXPECT_EQ(loaded->voted_for(), 3u);
  EXPECT_EQ(loaded->id(), 42u);

  EXPECT_FALSE(fs::exists(Path().string() + ".tmp"));
}

TEST_F(RaftLogMetaRecordTest, replaces_the_previous_record)
{
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), MakeState(1, 1, 1)));
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), MakeState(9, 8, 1)));

  auto loaded = RaftLogMetaRecord::Load(Path().string());
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded->term(), 9u);
  EXPECT_EQ(loaded->voted_for(), 8u);
  EXPECT_EQ(loaded->id(), 1u);
}

TEST_F(RaftLogMetaRecordTest, rejects_bad_magic)
{
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), MakeState(1, 2, 3)));

  auto data = ReadAll(Path());
  data[0] ^= 0x01;
  WriteAll(Path(), data);

  ExpectLoadError(Path(), E_INVALID_META_RECORD);
}

TEST_F(RaftLogMetaRecordTest, rejects_bad_version)
{
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), MakeState(1, 2, 3)));

  auto data = ReadAll(Path());
  data[sizeof(u32)] ^= 0x01;
  WriteAll(Path(), data);

  ExpectLoadError(Path(), E_INVALID_META_RECORD);
}

TEST_F(RaftLogMetaRecordTest, rejects_bad_size)
{
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), MakeState(1, 2, 3)));

  auto data = ReadAll(Path());
  le::EncodeU32(reinterpret_cast<u8 *>(&data[2 * sizeof(u32)]), 1);
  WriteAll(Path(), data);

  ExpectLoadError(Path(), E_INVALID_META_RECORD);
}

TEST_F(RaftLogMetaRecordTest, rejects_bad_checksum)
{
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), MakeState(1, 2, 3)));

  auto data = ReadAll(Path());
  data.back() ^= 0x01;
  WriteAll(Path(), data);

  ExpectLoadError(Path(), E_META_CHECKSUM_MISMATCH);
}

TEST_F(RaftLogMetaRecordTest, rejects_truncated_file)
{
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), MakeState(1, 2, 3)));

  std::error_code ec;
  fs::resize_file(Path(), RaftLogMetaRecord::kHeaderSize - 1, ec);
  ASSERT_FALSE(ec);

  ExpectLoadError(Path(), E_INVALID_META_RECORD);
}

TEST_F(RaftLogMetaRecordTest, rejects_trailing_bytes)
{
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), MakeState(1, 2, 3)));

  AppendBytes(Path(), std::string(1, '\0'));

  ExpectLoadError(Path(), E_INVALID_META_RECORD);
}

TEST_F(RaftLogMetaRecordTest, rejects_missing_file)
{
  ExpectLoadError(Path(), E_OPEN_META_FILE);
}

TEST_F(RaftLogMetaRecordTest, failed_store_keeps_the_previous_record)
{
  ASSERT_FALSE(RaftLogMetaRecord::Store(Path().string(), MakeState(4, 5, 6)));

  PersistentState incomplete;
  errcode = E_OK;
  auto err = RaftLogMetaRecord::Store(Path().string(), incomplete);
  ASSERT_TRUE(err);
  err.IgnoreCheck();
  EXPECT_EQ(errcode, E_SERIALIZE_META);

  auto loaded = RaftLogMetaRecord::Load(Path().string());
  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded->term(), 4u);
  EXPECT_EQ(loaded->voted_for(), 5u);
  EXPECT_EQ(loaded->id(), 6u);
}
