// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft/storage/raft_log_meta_record.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "kraft/error_code.h"
#include "kraft/storage/dir.h"
#include "kraft/storage/file.h"
#include "kraft/util/crc_codec_api.h"
#include "kraft/util/little_endian_codec_api.h"

using namespace kraft;
using namespace kerror;

namespace {

std::string ParentDir(std::string const &path)
{
  const auto pos = path.find_last_of('/');
  if (std::string::npos == pos) {
    return ".";
  }
  if (0 == pos) {
    return "/";
  }
  return path.substr(0, pos);
}

void RemoveQuietly(std::string const &path)
{
  if (0 != std::remove(path.c_str())) {
    // Best effort: the original error is more important.
  }
}

Error FsyncParentDir(std::string const &path)
{
  const auto dir_path = ParentDir(path);
  Dir dir;
  if (!dir.Open(dir_path)) {
    errcode = E_OPEN_DIR;
    return MakeMsgErrorf("Failed to open the meta directory '%s'",
                         dir_path.c_str());
  }

  if (!dir.Fsync()) {
    errcode = E_WRITE_FILE;
    return MakeMsgErrorf("Failed to sync the meta directory '%s': %s",
                         dir_path.c_str(), strerror(errno));
  }
  return MakeSuccess();
}

} // namespace

auto RaftLogMetaRecord::Store(std::string const &path,
                              PersistentState const &state) -> Error
{
  if (!state.IsInitialized()) {
    errcode = E_SERIALIZE_META;
    return MakeMsgErrorf("The persistent state is incomplete");
  }

  std::string payload;
  if (!state.SerializeToString(&payload)) {
    errcode = E_SERIALIZE_META;
    return MakeMsgErrorf("Failed to serialize the persistent state");
  }

  if (payload.empty() || payload.size() > kMaxPayloadSize) {
    errcode = E_INVALID_META_RECORD;
    return MakeMsgErrorf("The serialized persistent state has an invalid size");
  }

  const auto size = static_cast<u32>(payload.size());
  std::vector<u8> record(kHeaderSize + size);
  le::EncodeU32(record.data(), kMagic);
  le::EncodeU32(record.data() + sizeof(u32), kVersion);
  le::EncodeU32(record.data() + 2 * sizeof(u32), size);
  le::EncodeU32(record.data() + 3 * sizeof(u32),
            crc::ComputeRecord(size, payload.data()));
  std::memcpy(record.data() + kHeaderSize, payload.data(), payload.size());

  // The temporary file lives in the same directory as the target, so rename()
  // is atomic. The fixed suffix assumes a single writer, which matches the
  // Raft driver's single-threaded access.
  const auto temp_path = path + ".tmp";
  {
    File file;
    if (!file.Open(temp_path, File::TRUNC | File::BIN)) {
      errcode = E_OPEN_META_FILE;
      return MakeMsgErrorf("Failed to open the temporary meta file '%s': %s",
                           temp_path.c_str(), strerror(errno));
    }

    if (file.Write(record.data(), record.size()) != record.size()) {
      errcode = E_WRITE_FILE;
      RemoveQuietly(temp_path);
      return MakeMsgErrorf("Failed to write the temporary meta file '%s'",
                           temp_path.c_str());
    }

    if (!file.Fsync()) {
      errcode = E_WRITE_FILE;
      RemoveQuietly(temp_path);
      return MakeMsgErrorf("Failed to sync the temporary meta file '%s': %s",
                           temp_path.c_str(), strerror(errno));
    }
  }

  if (0 != ::rename(temp_path.c_str(), path.c_str())) {
    const int saved_errno = errno;
    RemoveQuietly(temp_path);
    errcode = E_WRITE_FILE;
    return MakeMsgErrorf("Failed to replace the meta file '%s': %s",
                         path.c_str(), strerror(saved_errno));
  }

  // Make the rename itself durable. The record is already visible, so an error
  // here means only that the replacement may not survive a crash.
  return FsyncParentDir(path);
}

auto RaftLogMetaRecord::Load(std::string const &path)
    -> ErrorOr<PersistentState>
{
  File file;
  if (!file.Open(path, File::READ | File::BIN)) {
    errcode = E_OPEN_META_FILE;
    return MakeMsgErrorf("Failed to open the meta file '%s': %s", path.c_str(),
                         strerror(errno));
  }

  const auto file_size = file.GetFileSize();
  if (File::INVALID_RETURN == file_size) {
    errcode = E_READ_FILE;
    return MakeMsgErrorf("Failed to get the size of the meta file '%s'",
                         path.c_str());
  }

  if (file_size < kHeaderSize) {
    errcode = E_INVALID_META_RECORD;
    return MakeMsgErrorf("The meta file '%s' is truncated", path.c_str());
  }

  u8 header[kHeaderSize] = {};
  auto ret = file.Read(header, sizeof header);
  if (File::INVALID_RETURN == ret || ret != sizeof header) {
    errcode = E_READ_FILE;
    return MakeMsgErrorf("Failed to read the header of the meta file '%s'",
                         path.c_str());
  }

  const auto magic = le::DecodeU32(header);
  const auto version = le::DecodeU32(header + sizeof(u32));
  const auto size = le::DecodeU32(header + 2 * sizeof(u32));
  const auto expected_crc = le::DecodeU32(header + 3 * sizeof(u32));

  if (magic != kMagic || version != kVersion) {
    errcode = E_INVALID_META_RECORD;
    return MakeMsgErrorf("The meta file '%s' has an unsupported format",
                         path.c_str());
  }

  if (0 == size || size > kMaxPayloadSize) {
    errcode = E_INVALID_META_RECORD;
    return MakeMsgErrorf("The meta file '%s' has an invalid payload size",
                         path.c_str());
  }

  if (file_size != kHeaderSize + size) {
    errcode = E_INVALID_META_RECORD;
    return MakeMsgErrorf("The meta file '%s' has trailing or missing bytes",
                         path.c_str());
  }

  std::string payload(size, '\0');
  ret = file.Read(payload.data(), size);
  if (File::INVALID_RETURN == ret || ret != size) {
    errcode = E_READ_FILE;
    return MakeMsgErrorf("Failed to read the payload of the meta file '%s'",
                         path.c_str());
  }

  if (crc::ComputeRecord(size, payload.data()) != expected_crc) {
    errcode = E_META_CHECKSUM_MISMATCH;
    return MakeMsgErrorf("The checksum of the meta file '%s' is incorrect",
                         path.c_str());
  }

  PersistentState state;
  if (!state.ParseFromArray(payload.data(), static_cast<int>(size)) ||
      !state.IsInitialized())
  {
    errcode = E_INVALID_META_RECORD;
    return MakeMsgErrorf("Failed to parse the meta file '%s'", path.c_str());
  }

  return state;
}
