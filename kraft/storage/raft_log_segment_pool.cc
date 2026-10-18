// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft/storage/raft_log_segment_pool.h"

#include "kraft/error_code.h"

using namespace kraft;
using namespace kerror;

RaftLogSegmentPool::RaftLogSegmentPool() noexcept {}

RaftLogSegmentPool::~RaftLogSegmentPool() noexcept {}

auto RaftLogSegmentPool::Init(size_t sz) -> void
{
  segments_.resize(sz);
  for (auto &segment : segments_) {
    segment.iter_ = path_idx_map_.end();
  }

  free_indices_.reserve(sz);
  for (size_t i = 0; i < sz; ++i) {
    free_indices_.push_back(i);
  }
}

auto RaftLogSegmentPool::GetLogSegment(std::string const &path)
    -> ErrorOr<RaftLogSegment *>
{
  // Construct a dummy value to call emplace()
  // instead of calling find() then calling emplace().
  // This is redundant and naive.
  SegmentCtx ctx;
  ctx.fifo_iter = fifo_indices_.end();
  ctx.idx = INVALID_INDEX;

  const auto emplace_ret = path_idx_map_.emplace(path.c_str(), std::move(ctx));
  const auto does_exists = emplace_ret.second;
  auto path_idx_iter = emplace_ret.first;
  RaftLogSegmentWithIter *ret_segment = nullptr;

  if (!does_exists || (u64)-1 == path_idx_iter->second.idx) {
    // The segment is don't represent in the pool
    RaftLogSegmentWithIter new_segment;
    auto error = new_segment.Init(path.c_str(), ReadOnlyFlag::ON);
    if (error) {
      return error;
    }
    new_segment.iter_ = path_idx_iter;

    u64 index = GetSegmentIndex(path_idx_iter);

    ret_segment = &segments_[index];
    *ret_segment = std::move(new_segment);
  } else {
    // Hit, just return
    ret_segment = &segments_[path_idx_iter->second.idx];
    KRAFT_ASSERT1((u64)-1 != ret_segment->iter_->second.idx);
  }

  return ret_segment;
}

auto RaftLogSegmentPool::RemoveSegment(std::string const &path, u64 index,
                                      TruncateFlag truncate) -> Error
{
  auto path_idx_iter = path_idx_map_.find(path.c_str());
  if (!(path_idx_map_.end() == path_idx_iter ||
        path_idx_iter->second.idx == (u64)-1))
  {
    // Since segment is managed by pool, we must truncate it here.
    auto segment_idx = path_idx_iter->second.idx;
    auto &segment = segments_[segment_idx];
    if (INVALID_INDEX == index) {
      index = segment.GetStartIndex();
    }
    if (auto err = segment.TruncateAfter(index, TruncateFlag::ON == truncate)) {
      return MakeMsgErrorf("Failed to truncate the segment in %s\n",
                           path.c_str());
    }
    fifo_indices_.erase(path_idx_iter->second.fifo_iter);
    free_indices_.push_back(segment_idx);
  }

  // The path is invalid now
  path_idx_map_.erase(path_idx_iter);
  return MakeSuccess();
}

auto RaftLogSegmentPool::AddSegment(std::string const &path,
                                   RaftLogSegment &segment) -> void
{
  // The path must be not added to this pool
  SegmentCtx ctx;
  ctx.fifo_iter = fifo_indices_.end();
  ctx.idx = INVALID_INDEX;

  const auto emplace_ret = path_idx_map_.emplace(path.c_str(), std::move(ctx));
  auto path_idx_iter = emplace_ret.first;

  auto index = GetSegmentIndex(path_idx_iter);
  RaftLogSegmentWithIter segment_iter;
  (RaftLogSegment &)segment_iter = std::move(segment);
  segment_iter.iter_ = std::move(path_idx_iter);
  segments_[index] = std::move(segment_iter);
}
