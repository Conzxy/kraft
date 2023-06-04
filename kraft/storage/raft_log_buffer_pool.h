// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_STORAGE_RAFT_LOG_BUFFER_POOL_H__
#define _KRAFT_STORAGE_RAFT_LOG_BUFFER_POOL_H__

// At first, RaftLogBufferPool is a internal class of BufferPoolLog.
// To maintainess, I split it from BufferPoolLog.

#include <cstdlib> // size_t
#include <list>
#include <string>
#include <vector>

#include "kraft/macro.h"
#include "kraft/storage/raft_log_buffer.h"
#include "kraft/type.h"

namespace kraft {

class BufferPoolLog;

DEFINE_BOOLEAN_ENUM_CLASS(TruncateFlag);

class RaftLogBufferPool {
  friend class BufferPoolLog;

 public:
  RaftLogBufferPool() noexcept;
  ~RaftLogBufferPool() noexcept;
  KRAFT_DISABLE_COPY(RaftLogBufferPool);

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
   * \Param filename Must from the BufferPoolLog
   *
   * \Return
   *
   * \errcode
   *  see RaftLogBuffer::Init()
   */
  ErrorOr<RaftLogBuffer *> GetLogBuffer(std::string const &filename);

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

  void AddBuffer(std::string const &path, RaftLogBuffer &buffer);

 private:
  struct RaftLogBufferPoolImpl;
  friend struct RaftLogBufferPoolImpl;

  std::list<u64> fifo_indices_;

  struct BufferCtx {
    size_t idx;
    decltype(fifo_indices_)::iterator fifo_iter;
  };

  HashMap<char const *, BufferCtx> path_idx_map_;
  using PathIdxMapIter = decltype(path_idx_map_)::iterator;

  struct RaftLogBufferWithIter : public RaftLogBuffer {
    // using RaftLogBuffer::RaftLogBuffer;
    using RaftLogBuffer::RaftLogBuffer;

    decltype(path_idx_map_)::iterator iter_;
  };

  std::vector<RaftLogBufferWithIter> buffers_;

  std::vector<u64> free_indices_;
};
} // namespace kraft
#endif
