#include "kraft/storage/raft_log_buffer_pool.h"

using namespace kraft;

struct RaftLogBufferPool::RaftLogBufferPoolImpl {
  KRAFT_INLINE static u64 GetVictimIndex(RaftLogBufferPool *me)
  {
    KRAFT_ASSERT(me->free_indices_.empty(), "There are no free index");
    auto ret = me->fifo_indices_.front();
    me->fifo_indices_.splice(me->fifo_indices_.end(), me->fifo_indices_,
                             me->fifo_indices_.begin());
    KRAFT_ASSERT1(me->fifo_indices_.back() == ret);
    return ret;
  }

  KRAFT_INLINE static void UpdateVictim(RaftLogBufferPool *me, u64 index)
  {
    me->fifo_indices_.push_back(index);
    KRAFT_ASSERT(me->fifo_indices_.size() <= me->buffers_.size(),
                 "The victim index list size must be <= buffer num");
  }

  KRAFT_INLINE static void
  UpdateBufferAndMapIter(RaftLogBufferPool *me, u64 index, PathIdxMapIter iter)
  {
    // Register the old buffer idx to invalid
    auto *buffer = &me->buffers_[index];
    if (me->path_idx_map_.end() != buffer->iter_) {
      buffer->iter_->second.idx = -1;
    }

    // Update current iterator
    // index: for get buffer check exists
    // fifo_iter: for remove buffer to remove fifo entry and add to free indices
    if (iter != me->path_idx_map_.end()) {
      iter->second.idx = index;
      iter->second.fifo_iter = --me->fifo_indices_.end();
    }

    buffer->iter_ = iter;
  }

  KRAFT_INLINE static u64 GetBufferIndex(RaftLogBufferPool *me,
                                         PathIdxMapIter iter)
  {
    u64 index = -1;

    // First, check the free index
    if (me->free_indices_.size() > 0) {
      index = me->free_indices_.back();
      me->free_indices_.pop_back();

      RaftLogBufferPoolImpl::UpdateVictim(me, index);
    } else {
      // There is no free index, get a victim index according the replacement
      // policy.
      // Here, I use FIFO, it is enough for this case.
      index = RaftLogBufferPoolImpl::GetVictimIndex(me);
      KRAFT_ASSERT1((u64)-1 != index);
    }

    UpdateBufferAndMapIter(me, index, iter);
    return index;
  }
};
