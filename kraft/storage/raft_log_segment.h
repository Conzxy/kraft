// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_STORAGE_RAFT_LOG_BUFFER_H__
#define _KRAFT_STORAGE_RAFT_LOG_BUFFER_H__

// At first, RaftLogSegment is a internal class of RaftLogManager.
// To maintainess, I split it from RaftLogManager.

#include <cstdlib>

#include "kraft.pb.h"
#include "kraft/macro.h"
#include "kraft/storage/file.h"
#include "kraft/type.h"

#include <kerror/kerror.h>

namespace kraft {

using kerror::Error;
using kerror::ErrorOr;

class RaftLogManager;

DEFINE_BOOLEAN_ENUM_CLASS(ReadOnlyFlag);

/**
 * A log segment storage that contains log entries.
 * It can be configed to readonly and read-and-appendonly
 */
class RaftLogSegment {
  friend class RaftLogManager;

  using Entries = std::vector<Entry>;

 public:
  RaftLogSegment() noexcept;
  ~RaftLogSegment() noexcept;

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
    new (this) RaftLogSegment(path, readonly, &err);
    return err;
  }

  Error Init(std::string const &path, ReadOnlyFlag readonly)
  {
    Error err;
    new (this) RaftLogSegment(path.c_str(), readonly, &err);
    return err;
  }

  static ErrorOr<RaftLogSegment> Create(char const *path, ReadOnlyFlag readonly)
  {
    Error error;

    RaftLogSegment ret(path, readonly, &error);
    if (error) {
      return error;
    }
    return ret;
  }

  KRAFT_INLINE static ErrorOr<RaftLogSegment> Create(std::string const &path,
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

  struct LogName {
    u64 start_index = INVALID_INDEX;
    // INVALID_INDEX means this is a logging (appendable) file.
    u64 end_index = INVALID_INDEX;
  };

  /**
   * \brief Parse a log filename.
   *
   * Formats:
   *  [start_index]-[end_index].kraftlog   (sealed segment)
   *  [start_index]-logging.kraftlog       (appendable segment)
   *
   * \return false if the filename is not a valid kraft log name.
   */
  static bool ParseLogName(std::string const &path, LogName *name);

  /**
   * Support move to allow buffer can be swapped
   */
  RaftLogSegment(RaftLogSegment &&) noexcept = default;
  RaftLogSegment &operator=(RaftLogSegment &&) noexcept = default;

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

  /**
   * \brief Set the maximum number of entries this segment can hold.
   *
   * For a readonly segment it is also the expected entry count derived from
   * the [start]-[end] filename.
   *
   * NOTE: This must not be implemented with std::vector::capacity(): reserve()
   * only guarantees capacity() >= n, so capacity() is not a reliable entry
   * count and would make IsFull()/FetchEntries() misbehave.
   */
  KRAFT_INLINE void SetEntriesNum(size_t n)
  {
    max_entries_ = n;
    entries_.reserve(n);
  }

  KRAFT_INLINE void SetStartIndex(u64 idx) noexcept { start_index_ = idx; }

  /*--------------------------------------------------*/
  /* Attributes getter                                */
  /*--------------------------------------------------*/

  KRAFT_INLINE size_t GetCapacitry() const noexcept { return max_entries_; }
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

  KRAFT_INLINE Entries::const_iterator begin() const noexcept
  {
    return entries_.cbegin();
  }

  KRAFT_INLINE Entries::const_iterator end() const noexcept
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
    // Rename in the same directory as the current file. Never rely on the
    // process working directory.
    const auto new_name = MakeSiblingPath(MakeLoggingName(start_index_));
    return file_.Rename(new_name.c_str());
  }

 private:
  KRAFT_INLINE u64 GetCurrentIndex() const noexcept
  {
    return entries_.empty() ? start_index_
                            : entries_.front().index() + entries_.size() - 1;
  }

  KRAFT_INLINE bool IsFull() const noexcept
  {
    return max_entries_ > 0 && entries_.size() >= max_entries_;
  }

  /**
   * \brief Make a path in the directory of the current log file.
   */
  KRAFT_INLINE std::string MakeSiblingPath(std::string const &name) const
  {
    const auto &self = file_.filename();
    const auto separator_pos = self.rfind('/');
    if (std::string::npos == separator_pos) {
      return name;
    }
    return self.substr(0, separator_pos + 1) + name;
  }

  /**
   * \warnign
   *  Please call Init() to initialize the object
   */
  RaftLogSegment(char const *path, ReadOnlyFlag readonly, Error *err);

  std::vector<Entry> entries_;
  // The maximum (or expected) number of entries, NOT entries_.capacity().
  size_t max_entries_ = 0;
  u64 start_index_;
  FileWithMeta file_;

#ifndef NDEBUG
  bool is_readonly_ = true;
#endif
};

} // namespace kraft

#endif
