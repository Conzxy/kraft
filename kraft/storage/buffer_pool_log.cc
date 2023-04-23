#include "buffer_pool_log.h"

#include <unistd.h>

#include "kraft/macro.h"
#include "kraft/storage/buffer_pool_log_impl.h"
#include "kraft/storage/dir.h"
#include "kraft/storage/file.h"
#include "kraft/util.h"
#include "kraft/util/string_util.h"

#include "kraft/error_code.h"

using namespace kerror;

BufferPoolLog::BufferPoolLog() {}

BufferPoolLog::~BufferPoolLog() noexcept {}

auto BufferPoolLog::Init(char const *dir_name, size_t buffer_capa, size_t n)
    -> Error
{
  buffer_capa_ = buffer_capa;

  Dir dir;
  dir_path_ = dir_name;

  if (dir_path_.back() != '/') {
    dir_path_ += '/';
  }

  if (!dir.Open(dir_name)) {
    errcode = E_OPEN_DIR;
    return MakeMsgErrorf("Failed to open dir: %s", dir_name);
  }

  std::vector<std::string> log_names;
  if (!dir.ApplyDirEntries([&log_names](DirEntry const *entry) {
        // Get the filename without dir prefix at first.
        std::string log_name = entry->d_name;
        if (DT_REG == entry->d_type &&
            std::string::npos != log_name.rfind(KRAFT_ENTRY_LOG_EXT))
        {
          log_names.emplace_back(entry->d_name);
        }
      }))
  {
    errcode = E_READ_DIR;
    return MakeMsgErrorf("Failed to read dir");
  }

  bool has_logging_file = false;
  // Parse the filename to
  // validate whether the file is a valid log of kraft
  for (auto &log_name : log_names) {
#ifndef NDEBUG
    printf("logname = %s\n", log_name.c_str());
#endif
    // Readonly log file
    if (std::string::npos == log_name.rfind("logging")) {
      // Format:
      // [start_index]-[end_index].kraftlog
      has_logging_file = true;
      auto separator_pos = log_name.find('-');
      u64 start_index = 0;
      if (std::string::npos == separator_pos) {
        errcode = E_INVALID_LOG_FILENAME;
        return MakeMsgErrorf("The log is invalid: %s", log_name.c_str());
      }

      if (!Raw2U64(log_name.data(), separator_pos, &start_index) ||
          ULLONG_MAX == start_index)
      {
        errcode = E_INVALID_LOG_FILENAME;
        return MakeMsgErrorf("The log is invalid: %s", log_name.c_str());
      }
      idx_path_map_.emplace(
          start_index,
          BufferPoolLogImpl::MakePath(this, log_name.c_str(), log_name.size()));
    } else {
      // Load to memory
      // Format:
      // [start index]-logging.kraftlog

      // FIXME Really?
      if (has_logging_file) continue;

      cur_path_ =
          BufferPoolLogImpl::MakePath(this, log_name.c_str(), log_name.size());
      if (!cur_buffer_.Init(cur_path_.c_str(), ReadOnlyFlag::OFF)) {
        errcode = E_OPEN_LOGGING_FILE;
      }

      cur_buffer_.SetEntriesNum(buffer_capa_);
      if (auto err =
              cur_buffer_.FetchEntriesBefore(cur_buffer_.GetStartIndex() + 1))
      {
        PErrorSys(err);
        return err;
      }
    }
  }

#ifndef NDEBUG
  for (auto const &idx_path : idx_path_map_) {
    printf("idx_paths: \n");
    printf("(%llu -> %s)\n", (ull)idx_path.first, idx_path.second.c_str());
  }
#endif
  // If there is no logging file or no log file,
  // consider init a current buffer
  if (!has_logging_file) {
    Error err;
    if (idx_path_map_.empty()) {
      err = cur_buffer_.Init("1" KRAFT_LOGGING_LOG_NAME_SUFFIX,
                             ReadOnlyFlag::OFF);
    } else {
      auto last_idx_path_iter = --idx_path_map_.end();
      err = cur_buffer_.Init(
          RaftLogBuffer::MakeLoggingName(last_idx_path_iter->first),
          ReadOnlyFlag::OFF);
    }

    if (err) {
      return err;
    }
    cur_buffer_.SetEntriesNum(buffer_capa_);
  }

  const auto meta_path = BufferPoolLogImpl::MakePath(
      this, KRAFT_META_LOG_NAME, sizeof(KRAFT_META_LOG_NAME) - 1);

  int meta_file_mode = File::READ | File::BIN;

  // If metafile doesn't exists, first create it.
  // The WRITE flag don't creat file if it does not exists.
  if (0 == access(meta_path.c_str(), F_OK)) {
    meta_file_mode |= File::WRITE;
  } else {
    meta_file_mode |= File::TRUNC;
  }

  if (!meta_file_.Open(meta_path, meta_file_mode)) {
    errcode = E_OPEN_META_FILE;
    return MakeMsgErrorf("Failed to open the kraft meta file: %s",
                         meta_path.c_str());
  }

  buffer_pool_.Init(n);
  return MakeSuccess();
}

auto BufferPoolLog::AppendEntry(Entry &entry) -> bool
{
  bool new_buffer = false;

  auto err = cur_buffer_.AppendEntry(entry, &new_buffer);
  if (err) {
    switch (errcode) {
      case E_SERIALIZE_ENTRY:
        PError(err);
        break;
      case E_WRITE_FILE:
      case E_RENAME_FULL_LOG_FILE:
        PErrorSys(err);
        break;
    }
    return false;
  }

  if (new_buffer) {
    // The start index must greater than the last item.
    // Make the call be O(1)
    auto idx_path_iter = idx_path_map_.emplace_hint(idx_path_map_.end(),
                                                    cur_buffer_.GetStartIndex(),
                                                    cur_buffer_.GetFilename());
    char new_name[KRAFT_FILE_NAME_LEN];
    auto old_end_idx = (unsigned long long)cur_buffer_.GetEndIndex();
    snprintf(new_name, sizeof new_name, "%llu-logging.kraftlog",
             old_end_idx + 1);
    // The buffer is the latest readonly one,
    // it is likely to be readen by followers
    buffer_pool_.AddBuffer(idx_path_iter->second, cur_buffer_);
    RaftLogBuffer new_buffer;
    new_buffer.Init(new_name, ReadOnlyFlag::OFF);

    KRAFT_ASSERT1(new_buffer.GetStartIndex() == old_end_idx + 1);
    new_buffer.SetEntriesNum(buffer_capa_);
    cur_buffer_ = std::move(new_buffer);
  }
  return true;
}

auto BufferPoolLog::UpdateState(PersistentState const &state) -> bool
{
  KRAFT_ASSERT(meta_file_.IsValid(),
               "The file handle of meta data must be valid");

  if (!state.SerializeToFileDescriptor(fileno(meta_file_.GetFileHandler()))) {
    perror("Failed to serialize meta state to the file");
    meta_file_.Rewind();
    return false;
  }
  meta_file_.Rewind();
  return true;
}

auto BufferPoolLog::FetchState(PersistentState *state) -> bool
{
  auto ret = state->ParseFromFileDescriptor(meta_file_.GetNativeHandler());
  meta_file_.Rewind();
  return ret;
}

auto BufferPoolLog::GetLastEntryMeta() -> EntryMeta
{
  RaftLogBuffer *buffer = nullptr;
  if (cur_buffer_.has_logged_num() > 0) {
    buffer = &cur_buffer_;
  } else {
    KRAFT_ASSERT1(idx_path_map_.size() >= 1);
    auto p_prev_last_buffer_or_err =
        buffer_pool_.GetLogBuffer((--idx_path_map_.end())->second);
    if (p_prev_last_buffer_or_err) {
      PErrorSys(p_prev_last_buffer_or_err.error());
      return {INVALID_TERM, INVALID_INDEX};
    }
    buffer = *p_prev_last_buffer_or_err;
    if (auto err = buffer->FetchEntries()) {
      PErrorSys(err);
      return {INVALID_TERM, INVALID_INDEX};
    }
  }
  return Entry2Meta(buffer->GetLastEntry());
}

auto BufferPoolLog::GetEntryMeta(u64 index) -> u64
{
  Entry const *p_entry = nullptr;
  if (!GetEntry(index, &p_entry)) {
    return INVALID_TERM;
  }

  KRAFT_ASSERT1(p_entry);
  return p_entry->term();
}

auto BufferPoolLog::TruncateAfter(u64 index) -> bool
{
  auto first_buffer_iter = idx_path_map_.upper_bound(index);
  if (idx_path_map_.end() == first_buffer_iter) {
    // This must a index belonging for cur_buffer_
    // since assertion

    if (auto err = cur_buffer_.TruncateAfter(index, false)) {
      PErrorSys(err);
      return false;
    }
  } else {
    auto buffer_iter = first_buffer_iter;
    --first_buffer_iter;
    for (; idx_path_map_.end() != buffer_iter;) {
      auto const &path = buffer_iter->second;
      if (auto err = buffer_pool_.RemoveBuffer(path)) {
        PErrorSys(err);
        return false;
      }
      if (0 != remove(path.c_str())) {
        fprintf(stderr, "Failed to remove file: %s\n", path.c_str());
        return false;
      }

      // std::map is based on nodes, this is OK.
      auto next_buffer_iter = buffer_iter;
      ++next_buffer_iter;
      idx_path_map_.erase(buffer_iter);
      buffer_iter = std::move(next_buffer_iter);
    }

    // Update cur_buffer_ to buffer located in @p index
    if (cur_buffer_.GetStartIndex() != first_buffer_iter->first) {
      auto p_buffer_or_err =
          buffer_pool_.GetLogBuffer(first_buffer_iter->second);
      if (p_buffer_or_err) {
        PErrorSys(p_buffer_or_err.error());
        return false;
      }
      auto &buffer = **p_buffer_or_err;
      if (auto err = buffer.FetchEntriesBefore(index + 1)) {
        PErrorSys(err);
        return false;
      }
      if (0 != remove(cur_buffer_.GetFilename().c_str())) {
        PSysError("Failed to remove the old logging file");
        return false;
      }

      if (auto err = buffer_pool_.RemoveBuffer(first_buffer_iter->second, index,
                                               TruncateFlag::ON))
      {
        PErrorSys(err);
        return false;
      }

      cur_buffer_ = std::move(buffer);
#ifndef NDEBUG
      cur_buffer_.is_readonly_ = false;
#endif
      if (!cur_buffer_.RenameToLogging()) {
        PSysError("Failed to rename the readonly buffer to logging log");
        return false;
      }
    }
  }
  return true;
}

auto BufferPoolLog::GetEntry(u64 index, Entry const **entry) -> bool
{
  KRAFT_ASSERT1(entry);

  if (cur_buffer_.GetStartIndex() <= index) {
    *entry = &cur_buffer_[index];
    return true;
  }

  auto idx_path_iter = idx_path_map_.upper_bound(index);
  --idx_path_iter;
  auto p_buffer_or_err = buffer_pool_.GetLogBuffer(idx_path_iter->second);
  if (p_buffer_or_err) {
    return false;
  }
  auto p_buffer = *p_buffer_or_err;

  if (auto err = p_buffer->FetchEntries()) {
    PErrorSys(err);
    return false;
  }
  *entry = &(*p_buffer)[index];
  return true;
}

auto BufferPoolLog::SetBufferCapacity(size_t n) -> void
{
  buffer_capa_ = n;
  cur_buffer_.SetEntriesNum(n);
}

auto BufferPoolLog::DebugPrint() -> void
{
  printf("The path_idx_map of pool: \n");
  for (auto const &path_idx : buffer_pool_.path_idx_map_) {
    printf("(%s -> %llu)\n", path_idx.first, (ull)path_idx.second.idx);
  }

  printf("The idx_path_map: \n");
  for (auto const &idx_path : idx_path_map_) {
    printf("(%llu -> %s)\n", (ull)idx_path.first, idx_path.second.c_str());
  }
}
