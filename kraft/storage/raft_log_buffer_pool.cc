// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft/storage/raft_log_buffer_pool_impl.h"

#include "kraft/error_code.h"

using namespace kerror;

RaftLogBufferPool::RaftLogBufferPool() noexcept {}

RaftLogBufferPool::~RaftLogBufferPool() noexcept {}

auto RaftLogBufferPool::Init(size_t sz) -> void
{
  buffers_.resize(sz);
  for (auto &buffer : buffers_) {
    buffer.iter_ = path_idx_map_.end();
  }

  free_indices_.reserve(sz);
  for (size_t i = 0; i < sz; ++i) {
    free_indices_.push_back(i);
  }
}

auto RaftLogBufferPool::GetLogBuffer(std::string const &path)
    -> ErrorOr<RaftLogBuffer *>
{
  // Construct a dummy value to call emplace()
  // instead of calling find() then calling emplace().
  // This is redundant and naive.
  BufferCtx ctx;
  ctx.fifo_iter = fifo_indices_.end();
  ctx.idx = INVALID_INDEX;

  const auto emplace_ret = path_idx_map_.emplace(path.c_str(), std::move(ctx));
  const auto does_exists = emplace_ret.second;
  auto path_idx_iter = emplace_ret.first;
  RaftLogBufferWithIter *ret_buffer = nullptr;

  if (!does_exists || (u64)-1 == path_idx_iter->second.idx) {
    // The buffer is don't represent in the pool
    RaftLogBufferWithIter new_buffer;
    auto error = new_buffer.Init(path.c_str(), ReadOnlyFlag::ON);
    if (error) {
      return error;
    }
    new_buffer.iter_ = path_idx_iter;

    u64 index = RaftLogBufferPoolImpl::GetBufferIndex(this, path_idx_iter);

    ret_buffer = &buffers_[index];
    *ret_buffer = std::move(new_buffer);
  } else {
    // Hit, just return
    ret_buffer = &buffers_[path_idx_iter->second.idx];
    KRAFT_ASSERT1((u64)-1 != ret_buffer->iter_->second.idx);
  }

  return ret_buffer;
}

auto RaftLogBufferPool::RemoveBuffer(std::string const &path, u64 index,
                                     TruncateFlag truncate) -> Error
{
  auto path_idx_iter = path_idx_map_.find(path.c_str());
  if (!(path_idx_map_.end() == path_idx_iter ||
        path_idx_iter->second.idx == (u64)-1))
  {
    // Since buffer is managed by pool, we must truncate it here.
    auto buffer_idx = path_idx_iter->second.idx;
    auto &buffer = buffers_[buffer_idx];
    if (INVALID_INDEX == index) {
      index = buffer.GetStartIndex();
    }
    if (auto err = buffer.TruncateAfter(index, TruncateFlag::ON == truncate)) {
      return MakeMsgErrorf("Failed to truncate the buffer in %s\n",
                           path.c_str());
    }
    fifo_indices_.erase(path_idx_iter->second.fifo_iter);
    free_indices_.push_back(buffer_idx);
  }

  // The path is invalid now
  path_idx_map_.erase(path_idx_iter);
  return MakeSuccess();
}

auto RaftLogBufferPool::AddBuffer(std::string const &path,
                                  RaftLogBuffer &buffer) -> void
{
  // The path must be not added to this pool
  BufferCtx ctx;
  ctx.fifo_iter = fifo_indices_.end();
  ctx.idx = INVALID_INDEX;

  const auto emplace_ret = path_idx_map_.emplace(path.c_str(), std::move(ctx));
  auto path_idx_iter = emplace_ret.first;

  auto index = RaftLogBufferPoolImpl::GetBufferIndex(this, path_idx_iter);
  RaftLogBufferWithIter buffer_iter;
  (RaftLogBuffer &)buffer_iter = std::move(buffer);
  buffer_iter.iter_ = std::move(path_idx_iter);
  buffers_[index] = std::move(buffer_iter);
}
