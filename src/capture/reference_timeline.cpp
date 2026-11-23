#include "reference_timeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace buncancel::capture {
namespace {
constexpr double ticks_per_second = 10000000.0;
}

ReferenceTimeline::ReferenceTimeline(const ReferenceTimelineConfig &config)
    : config_(config) {
  if (config.sample_rate < 8000 || config.sample_rate > 192000 ||
      config.max_block_frames == 0 || config.max_block_frames > 65536 ||
      config.history_frames < config.max_block_frames + 2 ||
      config.history_frames > 1920000 ||
      config.holdback_frames >= config.history_frames ||
      !std::isfinite(config.max_drift_ppm) || config.max_drift_ppm < 0 ||
      config.max_drift_ppm > 10000) {
    throw std::invalid_argument("invalid reference timeline configuration");
  }
  frames_.resize(config.history_frames);
}

void ReferenceTimeline::clear(TimelineStatus status) noexcept {
  begin_ = end_ = 0;
  previous_frames_ = 0;
  previous_qpc_ = 0;
  rate_ppm_ = 0;
  status_ = status;
  pending_reset_ = true;
  was_usable_ = false;
  have_stream_ = false;
}

void ReferenceTimeline::reset() noexcept {
  clear(TimelineStatus::empty);
  generation_ = 0;
  previous_qpc_ = 0;
}

double ReferenceTimeline::relative_time(std::uint64_t qpc) const noexcept {
  return qpc >= origin_ ? static_cast<double>(qpc - origin_)
                        : -static_cast<double>(origin_ - qpc);
}

ReferenceTimeline::Frame &ReferenceTimeline::at(std::uint64_t index) noexcept {
  return frames_[index % frames_.size()];
}

const ReferenceTimeline::Frame &
ReferenceTimeline::at(std::uint64_t index) const noexcept {
  return frames_[index % frames_.size()];
}

bool ReferenceTimeline::push(const ReferencePacket &packet) noexcept {
  // Old queued generations must not tear down a newly established clock.
  if (have_stream_ && packet.generation < generation_) {
    return false;
  }
  if (packet.frame_count == 0 ||
      packet.frame_count > ReferencePacket::max_frames ||
      (packet.channel_count != 1 && packet.channel_count != 2) ||
      packet.sample_rate < 8000 || packet.sample_rate > 192000 ||
      packet.qpc_position_100ns == 0 ||
      packet.device_position >
          std::numeric_limits<std::uint64_t>::max() - packet.frame_count) {
    clear(TimelineStatus::invalid_packet);
    return false;
  }
  for (std::uint32_t i = 0; i < packet.frame_count * packet.channel_count;
       ++i) {
    if (!std::isfinite(packet.samples[i]) || std::abs(packet.samples[i]) > 1) {
      clear(TimelineStatus::invalid_packet);
      return false;
    }
  }
  if (have_stream_ && packet.generation == generation_ &&
      !packet.discontinuity &&
      packet.device_position + packet.frame_count <= next_device_position_) {
    return false; // Entire duplicate/stale packet: do not reset good history.
  }
  if (have_stream_ &&
      (packet.generation != generation_ || packet.discontinuity ||
       packet.device_position != next_device_position_ ||
       packet.sample_rate != source_rate_)) {
    clear(TimelineStatus::discontinuity);
  }

  const double nominal_period = ticks_per_second / packet.sample_rate;
  double period = nominal_period;
  double start{};
  if (have_stream_) {
    if (packet.qpc_position_100ns <= previous_qpc_) {
      clear(TimelineStatus::clock_mismatch);
      return false;
    }
    const auto elapsed_frames =
        packet.device_position - previous_device_position_;
    const double observed = relative_time(packet.qpc_position_100ns);
    const double predicted = previous_time_ + elapsed_frames * period_;
    // Annotation noise is separate from packet delivery jitter. A clock jump
    // beyond 5 ms is unsafe; small annotation noise is fitted across packets.
    if (std::abs(observed - predicted) > ticks_per_second * 0.005) {
      clear(TimelineStatus::clock_mismatch);
      return false;
    }
    const double position =
        static_cast<double>(packet.device_position - first_device_position_);
    constexpr double alpha = 0.02;
    const double dx = position - mean_position_;
    const double dy = observed - mean_time_;
    mean_position_ += alpha * dx;
    mean_time_ += alpha * dy;
    position_variance_ = (1 - alpha) * (position_variance_ + alpha * dx * dx);
    position_time_covariance_ =
        (1 - alpha) * (position_time_covariance_ + alpha * dx * dy);
    const double fitted_period = position_time_covariance_ / position_variance_;
    const double fitted_ppm = (nominal_period / fitted_period - 1) * 1000000;
    // Once a full second establishes the clock, reject sustained excess drift.
    // A 0.5 ms annotation allowance shrinks with the measurement baseline.
    const double annotation_allowance = 5000.0 / (position * nominal_period);
    if (position >= source_rate_ &&
        std::abs(fitted_ppm) >
            config_.max_drift_ppm + annotation_allowance * 1000000) {
      clear(TimelineStatus::clock_mismatch);
      return false;
    }
    rate_ppm_ =
        std::clamp(fitted_ppm, -config_.max_drift_ppm, config_.max_drift_ppm);
    period_ = nominal_period / (1 + rate_ppm_ / 1000000);
    const double fitted_time =
        mean_time_ + (position - mean_position_) * period_;
    // Slew phase at at most a tenth of a source frame per packet. The
    // reference never jumps to a packet's noisy clock annotation.
    start = predicted + std::clamp((fitted_time - predicted) * 0.1,
                                   -nominal_period * 0.1, nominal_period * 0.1);
    period = (start - previous_time_) / elapsed_frames;
    // Finalize the preceding packet between two smoothed clock anchors. This
    // interpolates rate error without turning worker-delivery jitter into
    // reference drift. The last packet remains nominal/predicted until its
    // successor arrives; render never extrapolates beyond its final sample.
    for (std::uint32_t i = 0; i < previous_frames_; ++i) {
      const auto index = previous_begin_ + i;
      if (index >= begin_) {
        at(index).time = previous_time_ + i * period;
      }
    }
  } else {
    origin_ = packet.qpc_position_100ns;
    generation_ = packet.generation;
    source_rate_ = packet.sample_rate;
    first_device_position_ = packet.device_position;
    previous_time_ = 0;
    period_ = nominal_period;
    mean_position_ = mean_time_ = position_variance_ =
        position_time_covariance_ = 0;
    have_stream_ = true;
  }
  previous_begin_ = end_;
  previous_time_ = start;
  for (std::uint32_t i = 0; i < packet.frame_count; ++i) {
    const auto offset = i * packet.channel_count;
    const float sample =
        packet.channel_count == 2
            ? 0.5F * packet.samples[offset] + 0.5F * packet.samples[offset + 1]
            : packet.samples[offset];
    at(end_) = {start + i * period, sample};
    ++end_;
    if (end_ - begin_ > frames_.size()) {
      ++begin_;
    }
  }
  previous_frames_ = packet.frame_count;
  previous_device_position_ = packet.device_position;
  previous_qpc_ = packet.qpc_position_100ns;
  next_device_position_ = packet.device_position + packet.frame_count;
  return true;
}

TimelineResult
ReferenceTimeline::render(std::uint64_t microphone_start_qpc_100ns,
                          std::uint32_t frames, float *mono) noexcept {
  TimelineResult result;
  result.reference_delay_frames = config_.holdback_frames;
  result.source_rate_ppm = rate_ppm_;
  result.latest_packet_qpc_100ns = previous_qpc_;
  result.reset_required = pending_reset_;
  pending_reset_ = false;
  if (!mono || frames == 0 || frames > config_.max_block_frames ||
      microphone_start_qpc_100ns == 0) {
    result.status = TimelineStatus::invalid_request;
    result.reset_required = true;
    was_usable_ = false;
    return result;
  }
  std::fill_n(mono, frames, 0.0F);
  if (!have_stream_ || end_ - begin_ < 2) {
    result.status = status_;
    return result;
  }
  const double host_period = ticks_per_second / config_.sample_rate;
  const double start = relative_time(microphone_start_qpc_100ns) -
                       config_.holdback_frames * host_period;
  const double finish = start + (frames - 1) * host_period;
  if (start < at(begin_).time || finish > at(end_ - 1).time) {
    result.status = TimelineStatus::underrun;
    result.reset_required = result.reset_required || was_usable_;
    was_usable_ = false;
    return result;
  }
  // Binary search once per block, then walk forward at the source rate.
  auto left = begin_;
  auto right = end_ - 1;
  while (right - left > 1) {
    const auto middle = left + (right - left) / 2;
    if (at(middle).time <= start) {
      left = middle;
    } else {
      right = middle;
    }
  }
  double power{};
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double time = start + i * host_period;
    while (left + 1 < end_ - 1 && at(left + 1).time <= time) {
      ++left;
    }
    const auto &a = at(left);
    const auto &b = at(left + 1);
    const double fraction = (time - a.time) / (b.time - a.time);
    mono[i] = static_cast<float>(a.sample + fraction * (b.sample - a.sample));
    power += static_cast<double>(mono[i]) * mono[i];
    result.peak = std::max(result.peak, std::abs(mono[i]));
  }
  result.rms = static_cast<float>(std::sqrt(power / frames));
  result.usable = true;
  result.status = TimelineStatus::ready;
  was_usable_ = true;
  return result;
}

} // namespace buncancel::capture
