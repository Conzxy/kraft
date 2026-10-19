// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft/storage/raft_log_segment.h"

#include <cstring>
#include <numeric>
#include <unistd.h>

#include "kraft/error_code.h"
#include "kraft/util/crc_codec_api.h"
#include "kraft/util/little_endian_codec_api.h"
#include "kraft/util/string_util.h"

using namespace kraft;
using namespace kerror;

namespace {

void EncodeFileHeader(u8 *out)
{
  le::EncodeU32(out, RaftLogSegment::kFileMagic);
  le::EncodeU16(out + sizeof(u32), RaftLogSegment::kFileVersion);
  le::EncodeU16(out + sizeof(u32) + sizeof(u16), 0);
}

bool IsValidFileHeader(u8 const *header)
{
  return le::DecodeU32(header) == RaftLogSegment::kFileMagic &&
         le::DecodeU16(header + sizeof(u32)) == RaftLogSegment::kFileVersion &&
         le::DecodeU16(header + sizeof(u32) + sizeof(u16)) == 0;
}

} // namespace

RaftLogSegment::RaftLogSegment() noexcept {}

RaftLogSegment::~RaftLogSegment() noexcept {}

auto RaftLogSegment::ParseLogName(std::string const &path, LogName *name) -> bool
{
  KRAFT_ASSERT1(name);

  const auto filename = GetFilenameWithoutDir(path);
  const auto separator_pos = filename.find('-');
  // A u64 index has at most 20 decimal digits.
  if (0 == separator_pos || std::string::npos == separator_pos ||
      separator_pos > 20)
  {
    return false;
  }

  u64 start_index = 0;
  if (!Raw2U64(filename.data(), separator_pos, &start_index) ||
      INVALID_INDEX == start_index)
  {
    return false;
  }

  const auto extension_pos = filename.find('.', separator_pos + 1);
  if (std::string::npos == extension_pos ||
      filename.compare(extension_pos, std::string::npos,
                       KRAFT_ENTRY_LOG_EXT) != 0)
  {
    return false;
  }

  name->start_index = start_index;

  const auto index_part =
      filename.substr(separator_pos + 1, extension_pos - separator_pos - 1);
  if (index_part.empty() || index_part.size() > 20) {
    return false;
  }

  if ("logging" == index_part) {
    name->end_index = INVALID_INDEX;
    return true;
  }

  u64 end_index = 0;
  if (!Raw2U64(index_part.data(), index_part.size(), &end_index) ||
      INVALID_INDEX == end_index || end_index < start_index)
  {
    return false;
  }
  name->end_index = end_index;
  return true;
}

auto RaftLogSegment::InitAsSealed(std::string const &path) -> Error
{
  KRAFT_ASSERT1(nullptr == file_.fp());

  // A sealed segment must already exist; opening with "a+" would otherwise
  // create an empty file.
  if (0 != access(path.c_str(), F_OK)) {
    errcode = E_OPEN_LOG_FILE;
    return MakeMsgErrorf("The log file does not exist: %s", path.c_str());
  }

  FileWithMeta file;
  if (!file.Open(path.c_str(), File::READ | File::APP | File::BIN)) {
    errcode = E_OPEN_LOG_FILE;
    return MakeMsgErrorf("Failed to open log file: %s", path.c_str());
  }

  Error err;
  new (this) RaftLogSegment(std::move(file), OpenMode::kSealed, &err);
  return err;
}

auto RaftLogSegment::InitAsLogging(std::string const &path) -> Error
{
  KRAFT_ASSERT1(nullptr == file_.fp());

  LogName log_name;
  if (!ParseLogName(path, &log_name) ||
      INVALID_INDEX != log_name.end_index)
  {
    errcode = E_INVALID_LOG_FILENAME;
    return MakeMsgErrorf("The logging filename is invalid: %s", path.c_str());
  }

  // Use read+append so the same handle can still read the file after it is
  // sealed and moved to the pool.
  FileWithMeta file;
  if (!file.Open(path.c_str(), File::READ | File::APP | File::BIN)) {
    errcode = E_OPEN_LOG_FILE;
    return MakeMsgErrorf("Failed to open the log file: %s", path.c_str());
  }

  const auto file_size = file.GetFileSize();
  if (File::INVALID_RETURN == file_size) {
    errcode = E_READ_FILE;
    return MakeMsgErrorf("Failed to get the size of log file: %s",
                         path.c_str());
  }

  if (file_size < kFileHeaderSize) {
    if (0 != file_size) {
      errcode = E_INVALID_LOG_VERSION;
      return MakeMsgErrorf("The log file has an incomplete header: %s",
                           path.c_str());
    }

    // A missing or zero-length file starts with the file header. This is the
    // only write performed by initialization.
    u8 header[kFileHeaderSize] = {};
    EncodeFileHeader(header);
    if (file.Write(header, sizeof header) != sizeof header) {
      errcode = E_WRITE_FILE;
      return MakeMsgErrorf("Failed to write the log header: %s", path.c_str());
    }
    file.Flush();
    if (!file.Fsync()) {
      errcode = E_WRITE_FILE;
      return MakeMsgErrorf("Failed to sync the log header: %s", path.c_str());
    }
  }

  Error err;
  new (this) RaftLogSegment(std::move(file), OpenMode::kLogging, &err);
  return err;
}

RaftLogSegment::RaftLogSegment(FileWithMeta &&file, OpenMode mode, Error *error)
  : file_(std::move(file))
{
  KRAFT_ASSERT1(error);
  file_.Rewind();

  LogName log_name;
  if (!ParseLogName(file_.filename(), &log_name)) {
    *error = MakeMsgErrorf("The log filename is invalid: %s",
                           file_.filename().c_str());
    errcode = E_INVALID_LOG_FILENAME;
    return;
  }

  const bool is_sealed_name = (INVALID_INDEX != log_name.end_index);
  if ((OpenMode::kSealed == mode) != is_sealed_name) {
    *error = MakeMsgErrorf("The log filename does not match its mode: %s",
                           file_.filename().c_str());
    errcode = E_INVALID_LOG_FILENAME;
    return;
  }

  SetStartIndex(log_name.start_index);

  if (auto header_error = CheckFileHeader()) {
    *error = std::move(header_error);
    return;
  }

  if (OpenMode::kSealed == mode) {
    SetEntriesNum(log_name.end_index - log_name.start_index + 1);
  } else {
#ifndef NDEBUG
    is_readonly_ = false;
#endif
    // Recover the entries appended by a previous run.
    Error load_error = LoadLoggingEntries();
    if (load_error) {
      *error = std::move(load_error);
      return;
    }
  }
}

auto RaftLogSegment::CheckFileHeader() -> Error
{
  const auto file_size = file_.GetFileSize();
  if (File::INVALID_RETURN == file_size) {
    errcode = E_READ_FILE;
    return MakeMsgErrorf("Failed to get the size of log file: %s",
                         file_.filename().c_str());
  }

  if (file_size < kFileHeaderSize) {
    errcode = E_INVALID_LOG_VERSION;
    return MakeMsgErrorf(
        "The log file is empty or its header is incomplete: %s",
        file_.filename().c_str());
  }

  u8 header[kFileHeaderSize] = {};
  const auto ret = file_.Read(header, sizeof header);
  if (File::INVALID_RETURN == ret || ret < sizeof header) {
    errcode = E_READ_FILE;
    return MakeMsgErrorf("Failed to read the log header: %s",
                         file_.filename().c_str());
  }

  if (!IsValidFileHeader(header)) {
    errcode = E_INVALID_LOG_VERSION;
    return MakeMsgErrorf("The log file has an unsupported version: %s",
                         file_.filename().c_str());
  }

  return MakeSuccess();
}
                               std::vector<unsigned char> *entry_buf,
                               EntryReadResult *result) -> Error
{
  KRAFT_ASSERT1(entry_buf);
  KRAFT_ASSERT1(result);

  *result = EntryReadResult::kError;

  size_header_t size_header = 0;
  auto ret = file_.Read(&size_header, sizeof size_header);
  if (File::INVALID_RETURN == ret) {
    errcode = E_READ_FILE;
    return MakeMsgErrorf(
        "Failed to read the size header of entry index %llu in %s",
        (ull)expected_index, file_.filename().c_str());
  }

  if (ret < sizeof size_header) {
    *result = EntryReadResult::kEof;
    return MakeSuccess();
  }

  if (0 == size_header) {
    errcode = E_INVALID_LOG_ENTRY;
    return MakeMsgErrorf(
        "The size header of entry index %llu is zero in %s",
        (ull)expected_index, file_.filename().c_str());
  }

  if (size_header > kMaxEntrySize) {
    errcode = E_ENTRY_TOO_LARGE;
    return MakeMsgErrorf(
        "The size header (%u) of entry index %llu is too large in %s",
        size_header, (ull)expected_index, file_.filename().c_str());
  }

  entry_buf->resize(size_header);
  ret = file_.Read(entry_buf->data(), size_header);
  if (File::INVALID_RETURN == ret) {
    errcode = E_READ_FILE;
    return MakeMsgErrorf(
        "Failed to read the entry of entry index %llu in %s",
        (ull)expected_index, file_.filename().c_str());
  }

  if (ret < size_header) {
    errcode = E_ENTRY_INCOMPELTE;
    return MakeMsgErrorf("The entry %llu is incomplete in %s",
                         (ull)expected_index, file_.filename().c_str());
  }

  KRAFT_ASSERT1(entry_buf->size() == size_header);

  // Parse and verify the entry index.
  entries_.emplace_back();
  auto &new_entry = entries_.back();
  if (!new_entry.ParseFromArray(entry_buf->data(),
                                static_cast<int>(entry_buf->size())) ||
      !new_entry.IsInitialized())
  {
    entries_.pop_back();
    errcode = E_INVALID_LOG_ENTRY;
    return MakeMsgErrorf(
        "Failed to parse entry index %llu (size = %u) in %s",
        (ull)expected_index, size_header, file_.filename().c_str());
  }

  if (new_entry.index() != expected_index) {
    const auto actual_index = new_entry.index();
    entries_.pop_back();
    errcode = E_INVALID_LOG_ENTRY;
    return MakeMsgErrorf(
        "The entry index %llu is incorrect, it should be %llu in %s",
        (ull)actual_index, (ull)expected_index, file_.filename().c_str());
  }

  *result = EntryReadResult::kOk;
  return MakeSuccess();
}

auto RaftLogSegment::FetchEntriesBefore(u64 index) -> Error
{
  LogName log_name;
  const auto has_valid_name = ParseLogName(file_.filename(), &log_name);
  KRAFT_ASSERT1(has_valid_name);

  // A logging segment owns its entries in memory; nothing to fetch.
  if (has_valid_name && INVALID_INDEX == log_name.end_index) {
    return MakeSuccess();
  }

  // Sealed segments know their end index, so this must read an exact number of
  // records and must not probe EOF.
  KRAFT_ASSERT1(has_valid_name && INVALID_INDEX != log_name.end_index);

  KRAFT_ASSERT1(index > start_index_ && index <= start_index_ + GetCapacitry());
  const auto num = index - start_index_;

  // Always start at the first record; the caller's file position is not part
  // of this method's contract.
  entries_.clear();
  file_.SeekBegin(kFileHeaderSize);

  std::vector<unsigned char> entry_buf;
  for (u64 i = 0; i < num; ++i) {
    const auto expected_index = start_index_ + i;
    EntryReadResult result;
    if (auto err = ReadEntry(expected_index, &entry_buf, &result)) {
      return err;
    }

    if (EntryReadResult::kEof == result) {
      errcode = E_SIZE_HEADER_INCOMPLTE;
      return MakeMsgErrorf(
          "The sealed log file %s ended before entry index %llu",
          file_.filename().c_str(), (ull)expected_index);
    }

    if (EntryReadResult::kOk != result) {
      errcode = E_INVALID_LOG_ENTRY;
      return MakeMsgErrorf("Failed to read entry index %llu in %s",
                           (ull)expected_index, file_.filename().c_str());
    }
  }

  return MakeSuccess();
}

auto RaftLogSegment::LoadLoggingEntries() -> Error
{
  LogName log_name;
  const auto has_valid_name = ParseLogName(file_.filename(), &log_name);
  KRAFT_ASSERT1(has_valid_name);
  KRAFT_ASSERT1(has_valid_name && INVALID_INDEX == log_name.end_index);

  entries_.clear();
  file_.SeekBegin(kFileHeaderSize);

  const auto file_size = file_.GetFileSize();
  if (File::INVALID_RETURN == file_size) {
    errcode = E_READ_FILE;
    return MakeMsgErrorf("Failed to get the size of %s",
                         file_.filename().c_str());
  }

  std::vector<unsigned char> entry_buf;
  for (;;) {
    const auto position = file_.GetCurrentPosition();
    if (position < 0) {
      errcode = E_READ_FILE;
      return MakeMsgErrorf("Failed to get the position of %s",
                           file_.filename().c_str());
    }

    const auto offset = static_cast<size_t>(position);
    if (offset == file_size) {
      break;
    }

    // A torn write may leave a partial size header at the tail. Everything
    // from here on is uncommitted; stop at the last complete entry.
    // 在Read之前检测header是否不完整，可以让文件指针停在最后一个完整的entry之后，
    // 避免Read时返回Eof丢失边界，方便直接truncate掉不完整的entry
    if (offset + sizeof(size_header_t) > file_size) {
      break;
    }

    const auto expected_index = start_index_ + entries_.size();
    EntryReadResult result;
    if (auto err = ReadEntry(expected_index, &entry_buf, &result)) {
      return err;
    }

    if (EntryReadResult::kEof == result) {
      break;
    }

    if (EntryReadResult::kOk != result) {
      errcode = E_INVALID_LOG_ENTRY;
      return MakeMsgErrorf("Failed to read entry index %llu in %s",
                           (ull)expected_index, file_.filename().c_str());
    }
  }

  return MakeSuccess();
}

auto RaftLogSegment::FetchEntries() -> Error
{
  LogName log_name;
  const auto has_valid_name = ParseLogName(file_.filename(), &log_name);
  KRAFT_ASSERT1(has_valid_name);

  // A logging segment owns its entries in memory; nothing to fetch.
  if (has_valid_name && INVALID_INDEX == log_name.end_index) {
    return MakeSuccess();
  }

  // FetchEntriesBefore() clears the in-memory entries and seeks past the file
  // header, so the current file position does not matter here.
  if (auto err = FetchEntriesBefore(start_index_ + GetCapacitry())) return err;

  if (entries_.empty()) {
    errcode = E_INVALID_LOG_CONTENT;
    return MakeMsgErrorf("The log file contains no entries: %s",
                         file_.filename().c_str());
  }

  const auto capa = entries_.back().index() - start_index_ + 1;
  if (capa != GetCapacitry()) {
    errcode = E_INVALID_LOG_CONTENT;
    return MakeMsgErrorf("The log file contains %zu entries but the filename "
                         "indicates it should has %zu entries",
                         capa, GetCapacitry());
  }
  return MakeSuccess();
}

auto RaftLogSegment::AppendEntry(Entry entry, bool *new_buffer) -> Error
{
#ifndef NDEBUG
  if (is_readonly_) {
    Panic("Readonly buffer can't append entry");
    // dummy
    return MakeSuccess();
  }
#endif

  if (!entry.has_index()) entry.set_index(1 + GetEndIndex());
  KRAFT_ASSERT1(new_buffer);
  *new_buffer = false;

  const size_header_t size_header = entry.ByteSizeLong();
  std::vector<u8> entry_buf(size_header + sizeof size_header);
  memcpy(&entry_buf[0], &size_header, sizeof size_header);
  if (!entry.SerializeWithCachedSizesToArray(&entry_buf[sizeof size_header])) {
    errcode = E_SERIALIZE_ENTRY;
    return MakeMsgErrorf("Failed to serialize entry [%llu] to %s",
                         GetCurrentIndex(),
                         file_.filename().c_str());
  }

  if (File::INVALID_RETURN == file_.Write(entry_buf.data(), entry_buf.size())) {
    errcode = E_WRITE_FILE;
    return MakeMsgErrorf("Failed to write entry binary to file");
  }

  if (!file_.Fsync()) {
    perror("Failed to sync log");
  }

  entries_.push_back(std::move(entry));

  if (IsFull()) {
    char name_we[KRAFT_FILE_NAME_LEN];
    snprintf(name_we, sizeof name_we, "%llu-%llu",
             (unsigned long long)start_index_,
             (unsigned long long)GetEndIndex());
    // Rename in the same directory as the logging file, not in the CWD.
    const auto new_name = MakeSiblingPath(MakeLogName(name_we));
    if (!file_.Rename(new_name.c_str())) {
      errcode = E_RENAME_FULL_LOG_FILE;
      return MakeMsgErrorf("Failed to rename the logging file '%s' to '%s'",
                           file_.filename().c_str(), new_name.c_str());
    }
    *new_buffer = true;
  }
  return MakeSuccess();
}

auto RaftLogSegment::TruncateAfter(u64 index, bool is_remove) -> Error
{
  KRAFT_ASSERT(index >= start_index_ - 1 &&
                   index <= start_index_ + has_logged_num() - 1,
               "The index is out of range, can't to truncate");

  entries_.resize(index + 1 - start_index_);

  if (is_remove) {
    auto truncated_size = std::accumulate(
        entries_.begin(), entries_.end(), kFileHeaderSize,
        [](size_t ret, Entry const &entry) {
          return ret + kRecordHeaderSize + entry.ByteSizeLong();
        });
    if (0 != ftruncate(file_.GetNativeHandler(),
                       static_cast<off_t>(truncated_size))) {
      errcode = E_TRUNCATE;
      return MakeMsgErrorf("Failed to truncate file: %s to %zu bytes",
                           file_.filename().c_str(), truncated_size);
    }
  }
  return MakeSuccess();
}
