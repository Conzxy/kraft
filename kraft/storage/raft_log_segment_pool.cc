// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft/storage/raft_log_segment_pool.h"

#include "kraft/error_code.h"

using namespace kraft;
using namespace kerror;

RaftLogSegmentPool::RaftLogSegmentPool() noexcept {}

RaftLogSegmentPool::~RaftLogSegmentPool() noexcept {}

auto RaftLogSegmentPool::Init(size_t sz) -> void
{
  path_to_index_.clear();
  fifo_indices_.clear();
  free_indices_.clear();

  // Drop all previously cached segments (and close their files).
  segments_.clear();
  segments_.resize(sz);
  for (auto &slot : segments_) {
    slot.fifo_iter_ = fifo_indices_.end();
  }

  free_indices_.reserve(sz);
  for (size_t i = 0; i < sz; ++i) {
    free_indices_.push_back(i);
  }
}

auto RaftLogSegmentPool::GetLogSegment(std::string const &path)
    -> ErrorOr<RaftLogSegment *>
{
  // 先查找再考虑加载新的Segment，
  // 避免加载失败导致误记录path_idx_map_
  // 只有在加载成功后才会执行replacement policy，
  // 并更新相关数据结构（path_idx_map_，fifo_indices_，free_indices_）
  auto path_idx_iter = path_to_index_.find(path);
  if (path_to_index_.end() != path_idx_iter) {
    return &segments_[path_idx_iter->second].segment_;
  }

  // Load before touching the pool: a failed load must not evict a cached
  // segment.
  RaftLogSegment new_segment;
  if (auto error = new_segment.InitAsSealed(path)) {
    return error;
  }

  const auto index = GetSegmentIndex();
  if (INVALID_INDEX == index) {
    errcode = E_ALLOC;
    return MakeMsgErrorf("The segment pool is not initialized");
  }

  auto &slot = segments_[index];
  slot.segment_ = std::move(new_segment);
  path_to_index_.emplace(path, index);

  KRAFT_ASSERT1(slot.segment_.GetFilename() == path);
  return &slot.segment_;
}

auto RaftLogSegmentPool::RemoveSegment(std::string const &path, u64 index,
                                       TruncateFlag truncate) -> Error
{
  auto path_idx_iter = path_to_index_.find(path);
  if (path_to_index_.end() == path_idx_iter) {
    // Not resident: there is nothing to truncate.
    return MakeSuccess();
  }

  const auto slot_index = path_idx_iter->second;
  auto &segment = segments_[slot_index].segment_;

  // Without TruncateFlag::ON the segment is just dropped from the cache; the
  // caller (RaftLogManager) is going to delete the file or move the segment
  // out, so its in-memory entries need no adjustment.
  if (TruncateFlag::ON == truncate) {
    if (INVALID_INDEX == index) {
      index = segment.GetStartIndex();
    }
    if (auto err = segment.TruncateAfter(index, true)) {
      return MakeMsgErrorf("Failed to truncate the segment in %s\n",
                           path.c_str());
    }
  }

  fifo_indices_.erase(segments_[slot_index].fifo_iter_);
  free_indices_.push_back(slot_index);
  path_to_index_.erase(path_idx_iter);

  // Keep the segment object: RaftLogManager::TruncateAfter() may move it out
  // of the pool to promote it to the current logging segment.
  return MakeSuccess();
}

auto RaftLogSegmentPool::AddSegment(std::string const &path,
                                    RaftLogSegment &segment) -> void
{
  // The path must not be resident in this pool yet.
  const auto path_idx_iter = path_to_index_.find(path);
  KRAFT_ASSERT1(path_to_index_.end() == path_idx_iter);
  if (path_to_index_.end() != path_idx_iter) {
    return;
  }

  const auto index = GetSegmentIndex();
  KRAFT_ASSERT1(INVALID_INDEX != index);
  if (INVALID_INDEX == index) {
    return;
  }

  auto &slot = segments_[index];
  slot.segment_ = std::move(segment);
  path_to_index_.emplace(path, index);

  KRAFT_ASSERT1(slot.segment_.GetFilename() == path);
}
