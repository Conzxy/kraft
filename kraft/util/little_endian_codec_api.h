// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef KRAFT_UTIL_LITTLE_ENDIAN_CODEC_API_H__
#define KRAFT_UTIL_LITTLE_ENDIAN_CODEC_API_H__

#include "kraft/macro.h"
#include "kraft/type.h"

namespace kraft {
namespace le {

// Fixed little-endian encoding. Do not use memcpy: the in-memory layout of an
// integer depends on the host endianness, while bit shifts define the byte
// order explicitly.

// 固定编码为 little-endian
// 不要用memcpy，对于同一个整型，不同端序的内存布局是相反的，
// 所以memcpy得到的也是不同的结果
// 固定编码需要依靠逐位字节拷贝，因为整型位运算得到的结果是端序无关的
// 小端序是低位内存放低位数据

KRAFT_INLINE void EncodeU16(u8 *out, u16 value) noexcept
{
  out[0] = static_cast<u8>(value);
  out[1] = static_cast<u8>(value >> 8);
}

KRAFT_INLINE void EncodeU32(u8 *out, u32 value) noexcept
{
  out[0] = static_cast<u8>(value);
  out[1] = static_cast<u8>(value >> 8);
  out[2] = static_cast<u8>(value >> 16);
  out[3] = static_cast<u8>(value >> 24);
}

KRAFT_INLINE u16 DecodeU16(u8 const *in) noexcept
{
  return static_cast<u16>(in[0]) | (static_cast<u16>(in[1]) << 8);
}

KRAFT_INLINE u32 DecodeU32(u8 const *in) noexcept
{
  return static_cast<u32>(in[0]) | (static_cast<u32>(in[1]) << 8) |
         (static_cast<u32>(in[2]) << 16) | (static_cast<u32>(in[3]) << 24);
}

} // namespace le
} // namespace kraft

#endif
