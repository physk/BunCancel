#pragma once

#include "windows/reference_capture.hpp"

#include <cstdint>
#include <vector>

namespace buncancel::capture {

struct ReferenceTimelineConfig {
  std::uint32_t sample_rate{48000};
  std::uint32_t max_block_frames{2048};
  std::uint32_t history_frames{96000};
  std::uint32_t holdback_frames{960};
  double max_drift_ppm{2000};
};

enum class TimelineStatus {
  empty,
  ready,
  underrun,
  invalid_packet,
  discontinuity,
  clock_mismatch,
  invalid_request
};

struct TimelineResult {
  bool usable{};
  bool reset_required{};
  TimelineStatus status{TimelineStatus::empty};
  // Reference is this many HOST frames older than the mic. No mic samples are
  // delayed. Acoustic lag = DSP lag + this bias; paths shorter than the bias
  // require a causal-alignment guard in the DSP and may be unusable.
  double reference_delay_frames{};
  double source_rate_ppm{};
  float rms{};
  float peak{};
  std::uint64_t latest_packet_qpc_100ns{};
};

// Single audio-thread owner, after construction on the control thread. All
// storage is allocated by construction; push/render/reset never allocate, lock,
// wait or call OS APIs. Packet arrival time is deliberately irrelevant.
// Microphone timestamps name the FIRST sample of each block, on the same QPC
// 100 ns clock as WASAPI. The caller owns host-clock timestamp smoothing.
class ReferenceTimeline {
public:
  explicit ReferenceTimeline(const ReferenceTimelineConfig &config = {});
  void reset() noexcept;
  bool push(const ReferencePacket &packet) noexcept;
  // Output is mono, with stereo downmixed using equal 0.5 weights. A failed
  // render zeros the WHOLE output block; the caller must pass through the mic
  // and reset its adaptation when reset_required is set. No extrapolation.
  TimelineResult render(std::uint64_t microphone_start_qpc_100ns,
                        std::uint32_t frames, float *mono) noexcept;

private:
  struct Frame {
    double time{};
    float sample{};
  };
  void clear(TimelineStatus status) noexcept;
  double relative_time(std::uint64_t qpc) const noexcept;
  Frame &at(std::uint64_t index) noexcept;
  const Frame &at(std::uint64_t index) const noexcept;

  ReferenceTimelineConfig config_;
  std::vector<Frame> frames_;
  std::uint64_t begin_{};
  std::uint64_t end_{};
  std::uint64_t origin_{};
  std::uint64_t generation_{};
  std::uint64_t next_device_position_{};
  std::uint64_t previous_device_position_{};
  std::uint64_t previous_qpc_{};
  std::uint64_t previous_begin_{};
  std::uint32_t previous_frames_{};
  std::uint32_t source_rate_{};
  std::uint64_t first_device_position_{};
  double previous_time_{};
  double period_{};
  double mean_position_{};
  double mean_time_{};
  double position_variance_{};
  double position_time_covariance_{};
  double rate_ppm_{};
  TimelineStatus status_{TimelineStatus::empty};
  bool have_stream_{};
  bool pending_reset_{true};
  bool was_usable_{};
};

} // namespace buncancel::capture
