// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef KRAFT_UTIL_CRC_CODEC_API_H__
#define KRAFT_UTIL_CRC_CODEC_API_H__

#include <cstddef>

#include <crc32c/crc32c.h>

#include "kraft/macro.h"
#include "kraft/type.h"
#include "kraft/util/little_endian_codec_api.h"

namespace kraft {
namespace crc {

/**
 * \brief CRC32C of @p size bytes in @p data.
 */
KRAFT_INLINE u32 Compute(void const *data, size_t size) noexcept
{
  return crc32c::Crc32c(static_cast<u8 const *>(data), size);
}

/**
 * \brief Extend @p crc with @p size bytes in @p data.
 */
KRAFT_INLINE u32 Extend(u32 crc, void const *data, size_t size) noexcept
{
  return crc32c::Extend(crc, static_cast<u8 const *>(data), size);
}

/**
 * \brief CRC of a record whose CRC input is a little-endian u32 size followed
 *        by the payload.
 *
 * This matches the storage record formats:
 *   [u32 size][u32 crc32c][payload]
 */
KRAFT_INLINE u32 ComputeRecord(u32 size, void const *payload) noexcept
{
  u8 size_bytes[sizeof(u32)];
  le::EncodeU32(size_bytes, size);
  return Extend(Compute(size_bytes, sizeof size_bytes), payload, size);
}

} // namespace crc
} // namespace kraft

#endif
