#ifndef KRAFT_BUFFER_POOL_LOG_H__
#define KRAFT_BUFFER_POOL_LOG_H__

#include <cstring>
#include <map>

#include "kraft.pb.h"
#include "kraft/macro.h"
#include "kraft/storage/file.h"
#include "kraft/storage/raft_log_buffer.h"
#include "kraft/storage/raft_log_buffer_pool.h"
#include "kraft/type.h"

namespace kraft {

class BufferPoolLog {
 public:
  BufferPoolLog();
  ~BufferPoolLog() noexcept;
  KRAFT_DISABLE_COPY(BufferPoolLog);

  // FIXME move op

  /**
   * \brief Init the log engine from directory
   *
   * \param dir_name Allow the directory path no / at back
   *
   * \return
   */
  Error Init(char const *dir_name, size_t buffer_capa = 10000, size_t n = 1);

  bool AppendEntry(Entry &entry);
  bool UpdateState(PersistentState const &state);

  EntryMeta GetLastEntryMeta();
  u64 GetEntryMeta(u64 index);
  bool TruncateAfter(u64 index);
  bool GetEntry(u64 index, Entry const **entry);

  bool FetchState(PersistentState *state);
  void SetBufferCapacity(size_t n);

  void DebugPrint();

 private:
  struct BufferPoolLogImpl;
  friend struct BufferPoolLogImpl;
  friend class RaftLogBufferPool;

  std::string dir_path_;
  std::map<u64, std::string> idx_path_map_;
  RaftLogBufferPool buffer_pool_;
  size_t buffer_capa_;

  std::string cur_path_;
  RaftLogBuffer cur_buffer_;

  File meta_file_;
};

using BpLog = BufferPoolLog;

} // namespace kraft

#endif
