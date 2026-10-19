// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef KRAFT_STORAGE_RAFT_LOG_META_RECORD_H__
#define KRAFT_STORAGE_RAFT_LOG_META_RECORD_H__

#include <cstddef>
#include <string>

#include "kraft.pb.h"
#include "kraft/type.h"

#include <kerror/kerror.h>

namespace kraft {

using kerror::Error;
using kerror::ErrorOr;

/**
 * \brief The persistent Raft metadata stored in the `kraftmeta` file.
 *
 * On-disk format (all integers are little-endian):
 *
 *   [u32 magic][u32 version][u32 size][u32 crc32c][payload]
 *
 * - size is the payload length;
 * - crc32c covers the encoded size field and the payload;
 * - payload is a serialized PersistentState.
 *
 * Store() replaces the file atomically: it writes a temporary file in the
 * same directory, fsyncs it, renames it over the target and fsyncs the
 * directory. A reader therefore sees either the old or the new record.
 */
class RaftLogMetaRecord {
 public:
  // 'K' 'R' 'M' 'T' when written little-endian.
  static constexpr u32 kMagic = 0x544D524B;
  static constexpr u32 kVersion = 1;
  static constexpr size_t kHeaderSize = 4 * sizeof(u32);
  static constexpr u32 kMaxPayloadSize = 1024u * 1024u;

  /**
   * \brief Atomically replace @p path with a record containing @p state.
   *
   * \errcode
   *  E_SERIALIZE_META, E_OPEN_META_FILE, E_WRITE_FILE, E_OPEN_DIR
   */
  static Error Store(std::string const &path, PersistentState const &state);

  /**
   * \brief Read and validate the record at @p path.
   *
   * \errcode
   *  E_OPEN_META_FILE, E_READ_FILE, E_INVALID_META_RECORD,
   *  E_META_CHECKSUM_MISMATCH
   */
  static ErrorOr<PersistentState> Load(std::string const &path);
};

} // namespace kraft

#endif
