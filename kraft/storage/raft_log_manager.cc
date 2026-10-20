// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft/storage/raft_log_manager.h"

#include <cstdio>

#include "kraft/macro.h"
#include "kraft/storage/dir.h"
#include "kraft/storage/raft_log_meta_record.h"
#include "kraft/util.h"

#include "kraft/error_code.h"

using namespace kraft;
using namespace kerror;

RaftLogManager::RaftLogManager() {}

RaftLogManager::~RaftLogManager() noexcept {}

auto RaftLogManager::Init(char const *dir_name, size_t segment_capa, size_t n)
    -> Error
{
  segment_capa_ = segment_capa;

  Dir dir;
  dir_path_ = dir_name;

  if (dir_path_.empty() || dir_path_.back() != '/') {
    dir_path_ += '/';
  }

  if (!dir.Open(dir_name)) {
    errcode = E_OPEN_DIR;
    return MakeMsgErrorf("Failed to open dir: %s", dir_name);
  }

  std::vector<std::string> log_names;
  if (!dir.ApplyDirEntries([&log_names](DirEntry const &entry) {
        if (!entry.IsRegularFile()) return;

        std::string log_name = entry.name();
        const std::string ext = KRAFT_ENTRY_LOG_EXT;
        if (log_name.size() > ext.size() &&
            log_name.compare(log_name.size() - ext.size(), ext.size(), ext) ==
                0)
        {
          log_names.emplace_back(std::move(log_name));
        }
      }))
  {
    errcode = E_READ_DIR;
    return MakeMsgErrorf("Failed to read dir");
  }

  bool has_logging_file = false;
  // Parse the filenames to restore the index. A logging file can appear at any
  // position in the directory entries, so don't depend on the order.
  for (auto &log_name : log_names) {
#ifndef NDEBUG
    printf("logname = %s\n", log_name.c_str());
#endif
    const auto path = MakeFullPath(log_name);

    RaftLogSegment::LogName log_info;
    if (!RaftLogSegment::ParseLogName(path, &log_info)) {
      errcode = E_INVALID_LOG_FILENAME;
      return MakeMsgErrorf("The log is invalid: %s", log_name.c_str());
    }

    if (INVALID_INDEX != log_info.end_index) {
      // Sealed segment: [start_index]-[end_index].kraftlog
      idx_path_map_.emplace(log_info.start_index, path);
      continue;
    }

    // Logging segment: [start_index]-logging.kraftlog
    // There must be at most one, ignore the rest defensively.
    if (has_logging_file) continue;
    has_logging_file = true;

    cur_path_ = path;

    // Creates the header for a crashed/empty file, otherwise recovers the
    // entries appended by a previous run.
    if (auto err = cur_segment_.InitAsLogging(cur_path_)) {
      if (E_OK == errcode) errcode = E_OPEN_LOGGING_FILE;
      return err;
    }

    cur_segment_.SetEntriesNum(segment_capa_);
  }

#ifndef NDEBUG
  for (auto const &idx_path : idx_path_map_) {
    printf("idx_paths: \n");
    printf("(%llu -> %s)\n", (ull)idx_path.first, idx_path.second.c_str());
  }
#endif
  // If there is no logging file, create a new one. The new segment must start
  // right after the last sealed segment ([start]-[end]), not at its start
  // index; the latter would overlap with the sealed data.
  if (!has_logging_file) {
    u64 logging_start = 1;
    if (!idx_path_map_.empty()) {
      auto last_idx_path_iter = --idx_path_map_.end();
      RaftLogSegment::LogName last_log_name;
      if (!RaftLogSegment::ParseLogName(last_idx_path_iter->second,
                                        &last_log_name) ||
          INVALID_INDEX == last_log_name.end_index)
      {
        errcode = E_INVALID_LOG_FILENAME;
        return MakeMsgErrorf("The log filename is invalid: %s",
                             last_idx_path_iter->second.c_str());
      }
      logging_start = last_log_name.end_index + 1;
    }

    const auto logging_path =
        MakeFullPath(RaftLogSegment::MakeLoggingName(logging_start));
    if (auto err = cur_segment_.InitAsLogging(logging_path)) {
      PErrorSys(err);
      return err;
    }
    cur_segment_.SetEntriesNum(segment_capa_);
  }

  meta_path_ =
      MakeFullPath(KRAFT_META_LOG_NAME, sizeof(KRAFT_META_LOG_NAME) - 1);

  segment_pool_.Init(n);
  return MakeSuccess();
}

auto RaftLogManager::AppendEntry(Entry &entry) -> bool
{
  bool new_segment = false;

  auto err = cur_segment_.AppendEntry(entry, &new_segment);
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

  if (new_segment) {
    const auto old_end_idx = cur_segment_.GetEndIndex();
    const auto new_name = MakeFullPath(
        RaftLogSegment::MakeLoggingName(old_end_idx + 1));

    // Create the new segment before moving the old one into the pool, so a
    // failure here leaves the manager with a valid cur_segment_.
    RaftLogSegment new_segment;
    if (auto err = new_segment.InitAsLogging(new_name)) {
      PErrorSys(err);
      return false;
    }

    KRAFT_ASSERT1(new_segment.GetStartIndex() == old_end_idx + 1);
    new_segment.SetEntriesNum(segment_capa_);

    // The start index must greater than the last item.
    // Make the call be O(1)
    auto idx_path_iter = idx_path_map_.emplace_hint(idx_path_map_.end(),
                                                    cur_segment_.GetStartIndex(),
                                                    cur_segment_.GetFilename());
    // The segment is the latest readonly one,
    // it is likely to be readen by followers
    segment_pool_.AddSegment(idx_path_iter->second, cur_segment_);
    cur_segment_ = std::move(new_segment);
  }
  return true;
}

auto RaftLogManager::UpdateState(PersistentState const &state) -> bool
{
  KRAFT_ASSERT(!meta_path_.empty(), "Init() must be called first");

  if (auto err = RaftLogMetaRecord::Store(meta_path_, state)) {
    PErrorSys(err);
    return false;
  }
  return true;
}

auto RaftLogManager::FetchState(PersistentState *state) -> bool
{
  KRAFT_ASSERT1(state);
  KRAFT_ASSERT(!meta_path_.empty(), "Init() must be called first");

  auto loaded = RaftLogMetaRecord::Load(meta_path_);
  if (loaded) {
    PErrorSys(loaded.error());
    return false;
  }

  *state = std::move(*loaded);
  return true;
}

auto RaftLogManager::GetLastEntryMeta() -> EntryMeta
{
  RaftLogSegment *segment = nullptr;
  if (cur_segment_.has_logged_num() > 0) {
    segment = &cur_segment_;
  } else {
    KRAFT_ASSERT1(idx_path_map_.size() >= 1);
    auto p_prev_last_segment_or_err =
        segment_pool_.GetLogSegment((--idx_path_map_.end())->second);
    if (p_prev_last_segment_or_err) {
      PErrorSys(p_prev_last_segment_or_err.error());
      return {INVALID_TERM, INVALID_INDEX};
    }
    segment = *p_prev_last_segment_or_err;
    if (auto err = segment->FetchEntries()) {
      PErrorSys(err);
      return {INVALID_TERM, INVALID_INDEX};
    }
  }
  return Entry2Meta(segment->GetLastEntry());
}

auto RaftLogManager::GetEntryMeta(u64 index) -> u64
{
  Entry const *p_entry = nullptr;
  if (!GetEntry(index, &p_entry)) {
    return INVALID_TERM;
  }

  KRAFT_ASSERT1(p_entry);
  return p_entry->term();
}

auto RaftLogManager::TruncateAfter(u64 index) -> bool
{
  auto first_segment_iter = idx_path_map_.upper_bound(index);
  if (idx_path_map_.end() == first_segment_iter) {
    // This must a index belonging for cur_segment_
    // since assertion

    if (auto err = cur_segment_.TruncateAfter(index, false)) {
      PErrorSys(err);
      return false;
    }
  } else {
    auto segment_iter = first_segment_iter;
    --first_segment_iter;
    for (; idx_path_map_.end() != segment_iter;) {
      auto const &path = segment_iter->second;
      if (auto err = segment_pool_.RemoveSegment(path)) {
        PErrorSys(err);
        return false;
      }
      if (0 != remove(path.c_str())) {
        fprintf(stderr, "Failed to remove file: %s\n", path.c_str());
        return false;
      }

      // std::map is based on nodes, this is OK.
      auto next_segment_iter = segment_iter;
      ++next_segment_iter;
      idx_path_map_.erase(segment_iter);
      segment_iter = std::move(next_segment_iter);
    }

    // Update cur_segment_ to segment located in @p index
    if (cur_segment_.GetStartIndex() != first_segment_iter->first) {
      auto p_segment_or_err =
          segment_pool_.GetLogSegment(first_segment_iter->second);
      if (p_segment_or_err) {
        PErrorSys(p_segment_or_err.error());
        return false;
      }
      auto &segment = **p_segment_or_err;
      if (auto err = segment.FetchEntriesBefore(index + 1)) {
        PErrorSys(err);
        return false;
      }
      if (0 != remove(cur_segment_.GetFilename().c_str())) {
        PSysError("Failed to remove the old logging file");
        return false;
      }
      if (auto err = segment_pool_.RemoveSegment(first_segment_iter->second, index,
                                               TruncateFlag::ON))
      {
        PErrorSys(err);
        return false;
      }

      cur_segment_ = std::move(segment);

      // This sealed segment is about to be truncated and promoted to the
      // current logging segment.
      if (!cur_segment_.RenameToLogging()) {
        PSysError("Failed to rename the readonly segment to logging log");
        return false;
      }
    }
  }
  return true;
}

auto RaftLogManager::GetEntry(u64 index, Entry const **entry) -> bool
{
  KRAFT_ASSERT1(entry);

  if (cur_segment_.GetStartIndex() <= index) {
    *entry = &cur_segment_[index];
    return true;
  }

  auto idx_path_iter = idx_path_map_.upper_bound(index);
  --idx_path_iter;
  auto p_segment_or_err = segment_pool_.GetLogSegment(idx_path_iter->second);
  if (p_segment_or_err) {
    return false;
  }
  auto p_segment = *p_segment_or_err;

  if (auto err = p_segment->FetchEntries()) {
    PErrorSys(err);
    return false;
  }
  *entry = &(*p_segment)[index];
  return true;
}

auto RaftLogManager::SetSegmentCapacity(size_t n) -> void
{
  segment_capa_ = n;
  cur_segment_.SetEntriesNum(n);
}

auto RaftLogManager::DebugPrint() -> void
{
  printf("The segment pool cache: \n");
  for (auto const &path_idx : segment_pool_.path_to_index_) {
    printf("(%s -> %llu)\n", path_idx.first.c_str(), (ull)path_idx.second);
  }

  printf("The idx_path_map: \n");
  for (auto const &idx_path : idx_path_map_) {
    printf("(%llu -> %s)\n", (ull)idx_path.first, idx_path.second.c_str());
  } 
}
