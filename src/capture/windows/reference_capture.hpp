#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace buncancel::capture {

enum class EndpointKind { render_loopback, capture };

struct Endpoint {
  std::string id;
  std::string name;
  EndpointKind kind{EndpointKind::render_loopback};
};

struct CaptureConfig {
  std::string endpoint_id;
  EndpointKind kind{EndpointKind::render_loopback};
  std::uint32_t sample_rate{48000};
};

enum class CaptureStatus {
  stopped,
  starting,
  running,
  unsupported_format,
  rate_mismatch,
  device_error
};

struct ReferencePacket {
  static constexpr std::uint32_t max_frames = 512;
  // Native mono or stereo, interleaved; stereo is preserved for the consumer.
  std::array<float, max_frames * 2> samples{};
  std::uint32_t frame_count{};
  std::uint32_t channel_count{};
  std::uint32_t sample_rate{};
  std::uint64_t device_position{};
  // WASAPI's QPC timestamp, already converted to 100 ns units, not raw ticks.
  std::uint64_t qpc_position_100ns{};
  std::uint64_t generation{};
  bool discontinuity{};
};

class ReferenceCapture {
public:
  ReferenceCapture();
  ~ReferenceCapture();
  ReferenceCapture(const ReferenceCapture &) = delete;
  ReferenceCapture &operator=(const ReferenceCapture &) = delete;

  // Control-thread operations. Enumeration includes render and capture
  // endpoints explicitly; a virtual mix name never determines its endpoint
  // class.
  static std::vector<Endpoint> enumerate_endpoints();
  // True means a worker was launched. Device/format results arrive via
  // status(). start/stop must be serialized by the owner; neither belongs in
  // audio process.
  bool start(const CaptureConfig &config);
  void stop() noexcept;

  // One consumer only. No locks, allocation, waiting or Windows API calls.
  // Old generations are discarded after restart. A discontinuity requires the
  // consumer to reset alignment/adaptation before using subsequent samples.
  bool pop(ReferencePacket &packet) noexcept;
  CaptureStatus status() const noexcept;
  std::uint64_t dropped_packets() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace buncancel::capture
