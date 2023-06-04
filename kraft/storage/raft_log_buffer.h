// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_STORAGE_RAFT_LOG_BUFFER_H__
#define _KRAFT_STORAGE_RAFT_LOG_BUFFER_H__

// At first, RaftLogBuffer is a internal class of BufferPoolLog.
// To maintainess, I split it from BufferPoolLog.

#include <cstdlib>

#include "kraft.pb.h"
#include "kraft/macro.h"
#include "kraft/storage/file.h"
#include "kraft/type.h"

#include <kerror/kerror.h>

namespace kraft {

using kerror::Error;
using kerror::ErrorOr;

class BufferPoolLog;

DEFINE_BOOLEAN_ENUM_CLASS(ReadOnlyFlag);

/**
 * A buffer contains log entries.
 * It can be configed to readonly and read-and-appendonly
 */
class RaftLogBuffer {
  friend class BufferPoolLog;

  using Entrys = std::vector<Entry>;

 public:
  RaftLogBuffer() noexcept;
  ~RaftLogBuffer() noexcept;

  /**
   * \brief
   *
   * \param path
   * \param readonly
   *
   * \return
   *
   * \errcode
   *  * E_OPEN_LOG_FILE
   */
  Error Init(char const *path, ReadOnlyFlag readonly)
  {
    Error err;
    new (this) RaftLogBuffer(path, readonly, &err);
    return err;
  }

  Error Init(std::string const &path, ReadOnlyFlag readonly)
  {
    Error err;
    new (this) RaftLogBuffer(path.c_str(), readonly, &err);
    return err;
  }

  static ErrorOr<RaftLogBuffer> Create(char const *path, ReadOnlyFlag readonly)
  {
    Error error;

    RaftLogBuffer ret(path, readonly, &error);
    if (error) {
      return error;
    }
    return ret;
  }

  KRAFT_INLINE static ErrorOr<RaftLogBuffer> Create(std::string const &path,
                                                    ReadOnlyFlag readonly)
  {
    return Create(path.c_str(), readonly);
  }

  KRAFT_INLINE static std::string MakeLogName(char const *name_we)
  {
    return std::string(name_we) + KRAFT_ENTRY_LOG_EXT;
  }

  KRAFT_INLINE static std::string MakeLogName(std::string name_we)
  {
    return name_we + KRAFT_ENTRY_LOG_EXT;
  }

  KRAFT_INLINE static std::string MakeLoggingName(u64 start_idx)
  {
    return std::to_string(start_idx) + KRAFT_LOGGING_LOG_NAME_SUFFIX;
  }

  /**
   * Support move to allow buffer can be swapped
   */
  RaftLogBuffer(RaftLogBuffer &&) noexcept = default;
  RaftLogBuffer &operator=(RaftLogBuffer &&) noexcept = default;

  // readonly use
  Error FetchEntries();

  Error FetchEntriesBefore(u64 index);
  /**
   * \brief
   *
   * \Param entry
   * \Param new_buffer The buffer is full, notify BufferPool to create a new
   *                   buffer
   *
   * \Return
   *  false -- Failed to serialize entry or rename(when full)
   */
  Error AppendEntry(Entry entry, bool *new_buffer);

  /**
   * \brief
   *
   * \Param index Discard the entries after the index
   * \Param is_remove Remove the file when index is start_index - 1
   *
   * \Return
   */
  Error TruncateAfter(u64 index, bool is_remove);
  KRAFT_INLINE Error TruncateAll(bool is_remove)
  {
    return TruncateAfter(start_index_ - 1, is_remove);
  }

  // Don't use reserve()
  // If the capacity is over the argument, the capacity() may don't changed.
  KRAFT_INLINE void SetEntriesNum(size_t n) { entries_.reserve(n); }

  KRAFT_INLINE void SetStartIndex(u64 idx) noexcept { start_index_ = idx; }

  /*--------------------------------------------------*/
  /* Attributes getter                                */
  /*--------------------------------------------------*/

  KRAFT_INLINE size_t GetCapacitry() const noexcept
  {
    return entries_.capacity();
  }
  KRAFT_INLINE u64 has_logged_num() const noexcept { return entries_.size(); }
  KRAFT_INLINE u64 GetStartIndex() const noexcept { return start_index_; }
  KRAFT_INLINE u64 GetEndIndex() const noexcept
  {
    return start_index_ + has_logged_num() - 1;
  }
  KRAFT_INLINE std::string const &GetFilename() const noexcept
  {
    return file_.filename();
  }

  /*--------------------------------------------------*/
  /* Entry getter                                     */
  /*--------------------------------------------------*/

  KRAFT_INLINE Entry const &GetLastEntry() const noexcept
  {
    return entries_.back();
  }

  KRAFT_INLINE Entrys::const_iterator begin() const noexcept
  {
    return entries_.cbegin();
  }

  KRAFT_INLINE Entrys::const_iterator end() const noexcept
  {
    return entries_.cend();
  }

  KRAFT_INLINE Entry const &operator[](size_t idx) const noexcept
  {
    KRAFT_ASSERT(start_index_ != (u64)-1, "Don't load the log file?");
    KRAFT_ASSERT(idx >= start_index_, "The index must after the start_index_");
    KRAFT_ASSERT(idx - start_index_ < entries_.size(),
                 "The index is over the range");
    return entries_[idx - start_index_];
  }

  KRAFT_INLINE bool RenameToLogging() noexcept
  {
    const auto new_name = MakeLoggingName(start_index_);
    return file_.Rename(new_name.c_str());
  }

 private:
  using Self = RaftLogBuffer;

  struct RaftLogBufferImpl;
  friend struct RaftLogBufferImpl;

  /**
   * \warnign
   *  Please call Init() to initialize the object
   */
  RaftLogBuffer(char const *path, ReadOnlyFlag readonly, Error *err);

  std::vector<Entry> entries_;
  u64 start_index_;
  FileWithMeta file_;

#ifndef NDEBUG
  bool is_readonly_ = true;
#endif
};

} // namespace kraft

#endif
