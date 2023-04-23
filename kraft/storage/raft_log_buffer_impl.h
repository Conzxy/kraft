#include "kraft/storage/raft_log_buffer.h"

struct kraft::RaftLogBuffer::RaftLogBufferImpl {
  KRAFT_INLINE static u64 GetCurrentIndex(Self *self) noexcept
  {
    return self->entries_.size() == 0
               ? self->start_index_
               : self->entries_.front().index() + self->entries_.size() - 1;
  }

  KRAFT_INLINE static bool IsFull(Self *self) noexcept
  {
    return self->entries_.size() == self->GetCapacitry();
  }
};

