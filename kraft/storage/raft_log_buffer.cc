#include "kraft/storage/raft_log_buffer.h"

#include <numeric>
#include <unistd.h>

#include "kraft/storage/raft_log_buffer_impl.h"
#include "kraft/error_code.h"
#include "kraft/util/string_util.h"

using namespace kraft;
using namespace kerror;

RaftLogBuffer::RaftLogBuffer() noexcept {}

RaftLogBuffer::~RaftLogBuffer() noexcept {}

RaftLogBuffer::RaftLogBuffer(char const *path, ReadOnlyFlag readonly,
                             Error *error)
{
  // Even readonly is enable, the mode also contains append
  // since truncate may need swap the buffer to current buffer
  // that is appendable.
  //
  // This is a internal class, this is safe for author(I think)!
  int mode = File::BIN | File::READ | File::APP;

#ifndef NDEBUG
  is_readonly_ = (ReadOnlyFlag::ON == readonly);
#endif

  if (!file_.Open(path, mode)) {
    *error = MakeMsgErrorf("Failed to open log file: %s", path);
    kraft::errcode = E_OPEN_LOG_FILE;
    return;
  }

  auto file_path = GetFilenameWithoutDir(file_.filename());
  const auto separator_pos = file_path.find('-');
  KRAFT_ASSERT(std::string::npos != separator_pos,
               "The log filename is invalid, generation logic has error");
  u64 start_index = -1;
  auto ok = Raw2U64(file_path.data(), separator_pos, &start_index);
  KRAFT_UNUSED(ok);
  KRAFT_ASSERT(ok, "The start index is invalid");
  SetStartIndex(start_index);
  const auto extension_pos = file_path.find('.', separator_pos + 1);
  if (ReadOnlyFlag::ON == readonly) {
    u64 end_index;
    ok = Raw2U64(file_path.data() + separator_pos + 1,
                 extension_pos - separator_pos - 1, &end_index);
    KRAFT_ASSERT(ok, "The end index is invalid");
    SetEntriesNum(end_index - start_index + 1);
  }
}

auto RaftLogBuffer::FetchEntriesBefore(u64 index) -> Error
{
  const auto is_logging =
      (file_.filename().rfind("logging") != std::string::npos);

  KRAFT_ASSERT1(index > start_index_ && index <= start_index_ + GetCapacitry());
  const auto num = index - start_index_;

  size_header_t size_header = 0;
  std::vector<unsigned char> entry_buf;

  Entry entry;

  for (;;) {
    // TODO Caching
    auto ret = file_.Read(&size_header, sizeof size_header);
    if (File::INVALID_RETURN == ret) {
      kraft::errcode = E_READ_FILE;
      return MakeMsgErrorf(
          "Failed to read the size header of entry index = %llu\n",
          RaftLogBufferImpl::GetCurrentIndex(this));
    } else if (ret < sizeof size_header) {
      if (is_logging)
        break;
      else {
        kraft::errcode = E_SIZE_HEADER_INCOMPLTE;
        return MakeMsgErrorf("The read header in %llu is incomplete",
                             RaftLogBufferImpl::GetCurrentIndex(this));
      }
    }

    KRAFT_ASSERT1(size_header != 0 && size_header != (u32)-1);

    entry_buf.resize(size_header);
    ret = file_.Read(&entry_buf[0], size_header);
    if (File::INVALID_RETURN == ret) {
      errcode = E_READ_FILE;
      return MakeMsgErrorf("Failed to read the entry of entry index = %llu\n",
                           RaftLogBufferImpl::GetCurrentIndex(this));
    } else if (ret < size_header) {
      errcode = E_ENTRY_INCOMPELTE;
      return MakeMsgErrorf("The entry in %llu is incomplete",
                           RaftLogBufferImpl::GetCurrentIndex(this));
    }

    KRAFT_ASSERT1(entry_buf.size() == size_header);
    entries_.emplace_back();
    entries_.back().ParseFromArray(entry_buf.data(), entry_buf.size());
    if (entries_.size() == num && !is_logging) break;
  }

  KRAFT_ASSERT1(entries_.front().index() == start_index_);

  if (entries_.back().index() != GetEndIndex()) {
    errcode = E_INVALID_LOG_ENTRY;
    return MakeMsgErrorf(
        "The last entry index: %zu is incorrect, it should be %zu",
        entries_.back().index(), GetEndIndex());
  }
  return MakeSuccess();
}

auto RaftLogBuffer::FetchEntries() -> Error
{
  if (auto err = FetchEntriesBefore(start_index_ + GetCapacitry())) return err;
  const auto capa = entries_.back().index() - start_index_ + 1;
  if (capa != GetCapacitry()) {
    errcode = E_INVALID_LOG_CONTENT;
    return MakeMsgErrorf("The log file contains %zu entries but the filename "
                         "indicates it should has %zu entries",
                         capa, GetCapacitry());
  }
  return MakeSuccess();
}

auto RaftLogBuffer::AppendEntry(Entry entry, bool *new_buffer) -> Error
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
                         RaftLogBufferImpl::GetCurrentIndex(this),
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

  char new_name[KRAFT_FILE_NAME_LEN];
  if (RaftLogBufferImpl::IsFull(this)) {
    const ull end_idx = GetEndIndex();
    snprintf(new_name, sizeof new_name, "%llu-%llu.kraftlog",
             (unsigned long long)start_index_, end_idx);
    if (!file_.Rename(new_name)) {
      errcode = E_RENAME_FULL_LOG_FILE;
      return MakeMsgErrorf("Failed to rename the logging file '%s' to '%s'",
                           file_.filename().c_str(), new_name);
    }
    *new_buffer = true;
  }
  return MakeSuccess();
}

auto RaftLogBuffer::TruncateAfter(u64 index, bool is_remove) -> Error
{
  KRAFT_ASSERT(index >= start_index_ - 1 &&
                   index <= start_index_ + has_logged_num() - 1,
               "The index is out of range, can't to truncate");

  entries_.resize(index + 1 - start_index_);

  if (is_remove) {
    auto truncated_size = std::accumulate(
        entries_.begin(), entries_.end(), (size_t)0,
        [](size_t ret, Entry const &entry) {
          return ret + entry.ByteSizeLong() + sizeof(size_header_t);
        });
    if (0 != ftruncate(file_.GetNativeHandler(), truncated_size)) {
      errcode = E_TRUNCATE;
      return MakeMsgErrorf("Failed to truncate file: %s to %zu bytes",
                           file_.filename().c_str(), truncated_size);
    }
  }
  return MakeSuccess();
}
