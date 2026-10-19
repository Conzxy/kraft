// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_STORAGE_RAFT_LOG_BUFFER_H__
#define _KRAFT_STORAGE_RAFT_LOG_BUFFER_H__

// At first, RaftLogSegment is a internal class of RaftLogManager.
// To maintainess, I split it from RaftLogManager.

#include <cstdlib>
#include <vector>

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

  /**
   * \brief Create a new appendable segment and write its file header.
   *
   * Init() only opens existing files; creation is explicit so that a
   * constructor/Init never writes to disk as a side effect.
   *
   * The returned segment keeps the file handle used to write the header, so
   * the file is opened only once. Its entries are empty and its capacity is
   * left to the caller (SetEntriesNum()).
   *
   * Fails if @p path already exists and is not empty.
   */
  static ErrorOr<RaftLogSegment> CreateLoggingFile(std::string const &path);

  // Upper bound of a serialized entry. It protects the reader from corrupted
  // size headers turning into huge allocations; real Raft entries are far
  // smaller than this.
  static constexpr size_header_t kMaxEntrySize = 64u * 1024 * 1024;

  // On-disk format (all integers are little-endian):
  //
  //   File header: [u32 magic][u16 version][u16 flags]
  //   Record:      [u32 size][u32 crc32c][payload]
  //
  // size is the payload length. crc32c covers the encoded size field and the
  // payload (the crc field itself is excluded).
  // 
  // FileHeader是kraft log file的metadata：
  // - magic：识别raft log file
  // - version：文件更迭用
  // - flags：保留位，扩展用
  static constexpr u32 kFileMagic = 0x5446524B; // bytes 'K' 'R' 'F' 'T'
  static constexpr u16 kFileVersion = 1;
  static constexpr size_t kFileHeaderSize = sizeof(u32) + 2 * sizeof(u16);

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

  /**
   * \brief Load all entries of a sealed segment.
   *
   * These Fetch methods are for readonly (sealed) segments. A logging segment
   * already owns its entries in memory (recovered by Init(), appended by
   * AppendEntry()), so they are no-ops for it.
   */
  Error FetchEntries();

  /**
   * \brief Load the entries [start_index_, index) of a sealed segment.
   *
   * No-op for a logging segment; see FetchEntries().
   */
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
  enum class EntryReadResult {
    kOk,
    kEof,
    kError,
  };

  /**
   * \brief Validate the segment file header.
   *
   * On success the file position is just after the header.
   */
  Error CheckFileHeader();

  /**
   * \brief Read, validate and append one entry.
   *
   * \Param expected_index The index the entry must have.
   * \Param entry_buf Reusable buffer for the serialized entry.
   * \Param result kEof if the file ended before a complete size header.
   */
  Error ReadEntry(u64 expected_index, std::vector<unsigned char> *entry_buf,
                  EntryReadResult *result);

  /**
   * \brief Recover the entries already present in an appendable (logging) file.
   *
   * Called once from the constructor. The file position is rewound first and
   * the file is read up to its current size; a trailing incomplete size header
   * (a torn write) is treated as the end of the valid log.
   */
  Error LoadLoggingEntries();

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
