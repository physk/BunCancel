#pragma once

#include "reference_capture.hpp"
#include "sample_format.hpp"

namespace buncancel::capture::detail {

// Builds one fragment of a valid native capture buffer. A timestamp failure
// invalidates alignment evidence, not the PCM used by the source meter. The
// caller preserves discontinuity after an unsuccessful queue push.
inline ReferencePacket reference_fragment(
    const std::uint8_t *data, const SampleFormat &format, std::uint32_t frames,
    std::uint32_t offset, std::uint64_t device_position,
    std::uint64_t qpc_position_100ns, std::uint64_t generation, bool silent,
    bool timestamp_valid, bool &discontinuity) noexcept {
  ReferencePacket packet;
  packet.frame_count = std::min(ReferencePacket::max_frames, frames - offset);
  packet.channel_count = format.channels;
  packet.sample_rate = format.sample_rate;
  packet.device_position = device_position + offset;
  timestamp_valid = timestamp_valid && qpc_position_100ns != 0;
  packet.qpc_position_100ns =
      timestamp_valid
          ? qpc_position_100ns + static_cast<std::uint64_t>(offset) *
                                     10000000ULL / format.sample_rate
          : 0;
  packet.generation = generation;
  packet.discontinuity = discontinuity || !timestamp_valid;
  if (!silent) {
    const auto sample_bytes = format.bits / 8;
    const auto sample_offset = offset * format.channels;
    const auto sample_count = packet.frame_count * format.channels;
    for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
      packet.samples[sample] =
          decode_sample(data + (sample_offset + sample) * sample_bytes, format);
    }
  }
  // The first subsequent valid packet also needs a reset, even if every bad
  // timestamp fragment was successfully queued and metered.
  discontinuity = !timestamp_valid;
  return packet;
}

} // namespace buncancel::capture::detail
