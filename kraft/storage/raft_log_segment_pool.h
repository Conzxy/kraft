// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_STORAGE_RAFT_LOG_SEGMENT_POOL_H__
#define _KRAFT_STORAGE_RAFT_LOG_SEGMENT_POOL_H__

// At first, RaftLogSegmentPool is a internal class of RaftLogManager.
// To maintainess, I split it from RaftLogManager.

#include <cstdlib> // size_t
#include <list>
#include <string>
#include <vector>

#include "kraft/macro.h"
#include "kraft/storage/raft_log_segment.h"
#include "kraft/type.h"

namespace kraft {

class RaftLogManager;

DEFINE_BOOLEAN_ENUM_CLASS(TruncateFlag);

class RaftLogSegmentPool {
  friend class RaftLogManager;

 public:
  RaftLogSegmentPool() noexcept;
  ~RaftLogSegmentPool() noexcept;
  KRAFT_DISABLE_COPY(RaftLogSegmentPool);

  /**
   * \brief
   *
   * \Param sz The maximum size of buffer this pool can hold
   *
   * \Return
   */
  void Init(size_t sz);

  /**
   * \brief Get the buffer in which the index belonging
   *
   * \Param filename Must from the RaftLogManager
   *
   * \Return
   *
   * \errcode
   *  see RaftLogSegment::Init()
   */
  ErrorOr<RaftLogSegment *> GetLogBuffer(std::string const &filename);

  /**
   * \brief Remove the buffer from pool
   *
   * \Param path
   *
   * \Return
   */
  Error RemoveBuffer(std::string const &path, u64 index, TruncateFlag truncate);

  Error RemoveBuffer(std::string const &path)
  {
    return RemoveBuffer(path, INVALID_INDEX, TruncateFlag::OFF);
  }

  void AddBuffer(std::string const &path, RaftLogSegment &buffer);

 private:
  std::list<u64> fifo_indices_;

  struct BufferCtx {
    size_t idx;
    decltype(fifo_indices_)::iterator fifo_iter;
  };

  HashMap<char const *, BufferCtx> path_idx_map_;
  using PathIdxMapIter = decltype(path_idx_map_)::iterator;

  struct RaftLogBufferWithIter : public RaftLogSegment {
    // using RaftLogBuffer::RaftLogBuffer;
    using RaftLogSegment::RaftLogSegment;

    decltype(path_idx_map_)::iterator iter_;
  };

  std::vector<RaftLogBufferWithIter> buffers_;

  std::vector<u64> free_indices_;

  KRAFT_INLINE u64 GetVictimIndex()
  {
    KRAFT_ASSERT(free_indices_.empty(), "There are no free index");
    auto ret = fifo_indices_.front();
    fifo_indices_.splice(fifo_indices_.end(), fifo_indices_,
                         fifo_indices_.begin());
    KRAFT_ASSERT1(fifo_indices_.back() == ret);
    return ret;
  }

  KRAFT_INLINE void UpdateVictim(u64 index)
  {
    fifo_indices_.push_back(index);
    KRAFT_ASSERT(fifo_indices_.size() <= buffers_.size(),
                 "The victim index list size must be <= buffer num");
  }

  KRAFT_INLINE void UpdateBufferAndMapIter(u64 index, PathIdxMapIter iter)
  {
    // Register the old buffer idx to invalid
    auto *buffer = &buffers_[index];
    if (path_idx_map_.end() != buffer->iter_) {
      buffer->iter_->second.idx = -1;
    }

    // Update current iterator
    // index: for get buffer check exists
    // fifo_iter: for remove buffer to remove fifo entry and add to free indices
    if (iter != path_idx_map_.end()) {
      iter->second.idx = index;
      iter->second.fifo_iter = --fifo_indices_.end();
    }

    buffer->iter_ = iter;
  }

  KRAFT_INLINE u64 GetBufferIndex(PathIdxMapIter iter)
  {
    u64 index = -1;

    // First, check the free index
    if (free_indices_.size() > 0) {
      index = free_indices_.back();
      free_indices_.pop_back();

      UpdateVictim(index);
    } else {
      // There is no free index, get a victim index according the replacement
      // policy.
      // Here, I use FIFO, it is enough for this case.
      index = GetVictimIndex();
      KRAFT_ASSERT1((u64)-1 != index);
    }

    UpdateBufferAndMapIter(index, iter);
    return index;
  }
};
} // namespace kraft
#endif
