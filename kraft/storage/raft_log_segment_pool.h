// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_STORAGE_RAFT_LOG_SEGMENT_POOL_H__
#define _KRAFT_STORAGE_RAFT_LOG_SEGMENT_POOL_H__

// At first, RaftLogSegmentPool is a internal class of RaftLogManager.
// To maintainess, I split it from RaftLogManager.

#include <cstdlib> // size_t
#include <iterator>
#include <list>
#include <string>
#include <vector>

#include "kraft/macro.h"
#include "kraft/storage/raft_log_segment.h"
#include "kraft/type.h"

namespace kraft {

class RaftLogManager;

DEFINE_BOOLEAN_ENUM_CLASS(TruncateFlag);

/**
 * \brief A fixed-size cache of sealed (readonly) log segments.
 *
 * RaftLogManager owns the Raft semantics: it resolves an entry index to a
 * segment path (idx_path_map_). This pool is a pure path-keyed file cache: it
 * resolves a path to a resident segment, or loads it from disk.
 *
 * The cache index is a value-semantics path -> slot map. Only the slot number
 * is stored, so it is safe across rehash and does not depend on the lifetime of
 * the caller's string (unlike the old char const* key). A slot removed from the
 * cache always has its map entry erased before the slot can be reused.
 *
 * The slots are:
 *  - segments_: resident segments (and the last moved-from value of a slot)
 *  - free_indices_: slots that are not resident
 *  - fifo_indices_: resident slots in reuse order; the front is the next victim
 */
class RaftLogSegmentPool {
  friend class RaftLogManager;

 public:
  RaftLogSegmentPool() noexcept;
  ~RaftLogSegmentPool() noexcept;
  KRAFT_DISABLE_COPY(RaftLogSegmentPool);

  /**
   * \brief Set the maximum number of segments this pool can hold.
   */
  void Init(size_t sz);

  /**
   * \brief Get the resident segment of @p filename, or load it from disk.
   *
   * \Param filename Must be a path from the RaftLogManager.
   *
   * \Return
   *  The returned pointer is valid until the slot is evicted by another
   *  GetLogSegment() call or removed by RemoveSegment().
   *
   * \errcode
   *  see RaftLogSegment::Init()
   */
  ErrorOr<RaftLogSegment *> GetLogSegment(std::string const &filename);

  /**
   * \brief Remove the segment from the pool.
   *
   * The segment object is left in place (only its cache entry is removed)
   * because the caller may move it out right after this call; see
   * RaftLogManager::TruncateAfter().
   *
   * \Param path
   * \Param index Discard the entries after the index; only used when
   *               @p truncate is ON
   * \Param truncate Also truncate the backing file. The in-memory entries must
   *                  already be loaded (see RaftLogManager::TruncateAfter()).
   *
   * \Return
   */
  Error RemoveSegment(std::string const &path, u64 index, TruncateFlag truncate);

  Error RemoveSegment(std::string const &path)
  {
    return RemoveSegment(path, INVALID_INDEX, TruncateFlag::OFF);
  }

  /**
   * \brief Add an already loaded segment to the pool.
   *
   * \Param path The segment path; it must equal segment.GetFilename().
   */
  void AddSegment(std::string const &path, RaftLogSegment &segment);

 private:
  struct Slot {
    RaftLogSegment segment_;
    // Only meaningful while the slot is resident (listed in fifo_indices_).
    std::list<u64>::iterator fifo_iter_;
  };

  /**
   * \brief Pick the next victim according to the replacement policy.
   *
   * The policy is FIFO: the front of fifo_indices_ is the oldest resident
   * segment. Move it to the back so it becomes the most recently used one.
   *
   * \Return INVALID_INDEX if there is no resident segment (pool not
   *         initialized).
   */
  KRAFT_INLINE u64 GetVictimIndex()
  {
    KRAFT_ASSERT(free_indices_.empty(), "There are no free index");
    if (fifo_indices_.empty()) {
      return INVALID_INDEX;
    }

    const auto victim = fifo_indices_.front();
    fifo_indices_.splice(fifo_indices_.end(), fifo_indices_,
                         fifo_indices_.begin());
    KRAFT_ASSERT1(fifo_indices_.back() == victim);
    KRAFT_ASSERT1(segments_[victim].fifo_iter_ == std::prev(fifo_indices_.end()));
    return victim;
  }

  /**
   * \brief Mark @p index as the most recently used slot.
   *
   * Used when a free slot is taken by a newly loaded segment.
   */
  KRAFT_INLINE void UpdateVictim(u64 index)
  {
    fifo_indices_.push_back(index);
    segments_[index].fifo_iter_ = std::prev(fifo_indices_.end());
    KRAFT_ASSERT(fifo_indices_.size() <= segments_.size(),
                 "The victim index list size must be <= segment num");
  }

  /**
   * \brief Drop the cache entry of a victim before its slot is overwritten.
   *
   * The victim is still resident here, so segment_.GetFilename() is the path
   * indexed in path_to_index_.
   */
  KRAFT_INLINE void InvalidateVictim(u64 index)
  {
    path_to_index_.erase(segments_[index].segment_.GetFilename());
  }

  /**
   * \brief Acquire a slot for a newly loaded segment.
   *
   * Reuse a free slot first, otherwise evict the FIFO front. The caller is
   * responsible for storing the new segment and inserting the new
   * path -> slot entry.
   *
   * \Return INVALID_INDEX if the pool is not initialized.
   */
  KRAFT_INLINE u64 GetSegmentIndex()
  {
    u64 index = INVALID_INDEX;

    if (!free_indices_.empty()) {
      index = free_indices_.back();
      free_indices_.pop_back();
      UpdateVictim(index);
    } else {
      index = GetVictimIndex();
      if (INVALID_INDEX == index) {
        return INVALID_INDEX;
      }
      InvalidateVictim(index);
    }

    return index;
  }

  // Only resident segments have an entry here.
  // 通过置换策略和相关函数把置换出去的segment从path_to_index_中删除，
  // 保证path_to_index_中只包含当前缓存的segment
  HashMap<std::string, u64> path_to_index_;
  std::list<u64> fifo_indices_;
  std::vector<Slot> segments_;
  std::vector<u64> free_indices_;
};

} // namespace kraft

#endif
