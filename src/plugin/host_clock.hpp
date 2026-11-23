#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace buncancel::plugin {

struct HostClockResult {
  std::uint64_t microphone_start_qpc_100ns{};
  bool reset_required{};
};

// Bridges the host sample clock to the WASAPI QPC epoch. Callback scheduling
// time is a noisy observation of the sample timeline, not its clock. No host
// systemTime is accepted: its epoch is not guaranteed to be Windows QPC.
class HostClock {
public:
  explicit HostClock(double sample_rate = 48000) noexcept
      : sample_rate_(sample_rate) {}

  void reset() noexcept {
    have_clock_ = false;
    have_position_ = false;
  }

  HostClockResult
  observe(std::uint64_t callback_qpc_100ns, std::uint32_t frames,
          bool continuous_valid = false,
          std::int64_t continuous_sample_position = 0) noexcept {
    if (!callback_qpc_100ns || !frames || sample_rate_ <= 0)
      return {};
    const bool host_jump = continuous_valid && have_position_ &&
                           continuous_sample_position != expected_position_;
    have_position_ = continuous_valid;
    if (continuous_valid)
      expected_position_ = continuous_sample_position + frames;
    const double block_duration =
        static_cast<double>(frames) * 10000000 / sample_rate_;
    // A live microphone input block has already been acquired when process()
    // runs. QPC therefore observes its end, rather than its first sample.
    const double observed_start =
        static_cast<double>(callback_qpc_100ns) - block_duration;
    if (observed_start <= 0)
      return {};
    const double error = observed_start - next_microphone_qpc_;
    const bool reanchor =
        !have_clock_ || host_jump || std::abs(error) > 1000000;
    if (reanchor) {
      next_microphone_qpc_ = observed_start;
      have_clock_ = true;
    } else {
      // Limit phase correction to 10 us/block. This follows slow clock error
      // while preserving continuous timing through ordinary burst scheduling.
      next_microphone_qpc_ += std::clamp(error * 0.001, -100.0, 100.0);
    }
    const auto result = static_cast<std::uint64_t>(next_microphone_qpc_);
    next_microphone_qpc_ +=
        static_cast<double>(frames) * 10000000 / sample_rate_;
    return {result, reanchor};
  }

private:
  double sample_rate_;
  double next_microphone_qpc_{};
  std::int64_t expected_position_{};
  bool have_clock_{};
  bool have_position_{};
};

} // namespace buncancel::plugin
