#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace buncancel::capture::detail {

enum class Encoding { floating_point, signed_pcm };

struct SampleFormat {
  Encoding encoding{Encoding::floating_point};
  std::uint16_t bits{32};
  std::uint16_t valid_bits{32};
  std::uint16_t channels{2};
  std::uint32_t sample_rate{48000};
};

inline bool valid_format(const SampleFormat &format) noexcept {
  if ((format.channels != 1 && format.channels != 2) ||
      format.sample_rate < 8000 || format.sample_rate > 192000) {
    return false;
  }
  if (format.encoding == Encoding::floating_point) {
    return format.bits == 32 && format.valid_bits == 32;
  }
  return (format.bits == 16 || format.bits == 24 || format.bits == 32) &&
         format.valid_bits > 0 && format.valid_bits <= format.bits;
}

// WASAPI PCM is little-endian and extensible valid bits are left-aligned.
inline float decode_sample(const std::uint8_t *data,
                           const SampleFormat &format) noexcept {
  if (format.encoding == Encoding::floating_point) {
    float value{};
    std::memcpy(&value, data, sizeof(value));
    return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
  }
  std::uint32_t word{};
  for (std::uint16_t byte = 0; byte < format.bits / 8; ++byte) {
    word |= static_cast<std::uint32_t>(data[byte]) << (byte * 8);
  }
  // Scale a signed two's-complement value without implementation-defined casts.
  const auto sign = std::uint32_t{1} << (format.bits - 1);
  const auto signed_value = static_cast<std::int64_t>(word & (sign - 1)) -
                            static_cast<std::int64_t>(word & sign);
  return static_cast<float>(static_cast<double>(signed_value) /
                            static_cast<double>(sign));
}

} // namespace buncancel::capture::detail
