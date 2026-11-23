#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace buncancel::plugin {

struct SignalMoments {
  double energy{};
  float peak{};
  std::uint64_t samples{};
};

struct ActivitySnapshot {
  float mic_rms{}, mic_peak{};
  float output_rms{}, output_peak{};
  float reference_rms{}, reference_peak{};
  float downmix_rms{}, downmix_peak{};
  double mic_seconds{}, reference_seconds{};
  float changed_fraction{}, usable_fraction{}, subtraction_fraction{};
  float mic_active_fraction{}, reference_active_fraction{};
  double reference_receipt_age_ms{};
  double mic_activity_age_ms{}, reference_activity_age_ms{};
  bool mic_observed{}, reference_observed{};
  bool reference_receipt_valid{};
  bool mic_activity_valid{}, reference_activity_valid{};
};

// One audio-thread owner. Stores sums and peaks, never audio. The trailing
// evidence window includes the current 50 ms bin and the preceding 59 bins.
// Its covered time is 2.95..3.00 seconds, with no extrapolation over gaps.
// Activity means an observed block/packet RMS above -60 dBFS. This diagnostic
// threshold never controls audio processing, readiness or cancellation.
class ActivityWindow {
public:
  static constexpr float activity_rms = 0.001F;

  void reset() noexcept { *this = {}; }

  void observeMicrophone(std::uint64_t receipt_qpc100ns, double sample_rate,
                         std::uint32_t frames, SignalMoments microphone,
                         SignalMoments output, std::uint32_t changed,
                         bool reference_usable,
                         bool subtraction_active) noexcept {
    if (!valid(receipt_qpc100ns, sample_rate, frames, microphone) ||
        !valid(receipt_qpc100ns, sample_rate, frames, output) ||
        microphone.samples != frames || output.samples != frames ||
        receipt_qpc100ns < lastMicrophoneReceipt_) {
      return;
    }
    auto &bin = at(receipt_qpc100ns);
    accumulate(bin.microphone, microphone);
    accumulate(bin.output, output);
    bin.micSeconds += frames / sample_rate;
    bin.changed += std::min(changed, frames);
    bin.usable += reference_usable ? frames : 0;
    bin.subtracting += subtraction_active ? frames : 0;
    lastMicrophoneReceipt_ = receipt_qpc100ns;
    if (active(microphone)) {
      bin.activeMicrophone += frames;
      lastMicrophoneActivity_ = receipt_qpc100ns;
    }
  }

  // Call once for each new packet (or its actual accumulated batch), using
  // arrival time on the callback clock. The packet's source timestamp can be
  // invalid independently of its useful PCM and does not determine meter age.
  void observeReference(std::uint64_t receipt_qpc100ns, double sample_rate,
                        std::uint32_t frames, SignalMoments raw,
                        SignalMoments mono) noexcept {
    if (!valid(receipt_qpc100ns, sample_rate, frames, raw) ||
        !valid(receipt_qpc100ns, sample_rate, frames, mono) ||
        mono.samples != frames ||
        (raw.samples != frames && raw.samples != 2ULL * frames) ||
        receipt_qpc100ns < lastReferenceReceipt_) {
      return;
    }
    auto &bin = at(receipt_qpc100ns);
    accumulate(bin.reference, raw);
    accumulate(bin.downmix, mono);
    bin.referenceSeconds += frames / sample_rate;
    lastReferenceReceipt_ = receipt_qpc100ns;
    if (active(raw)) {
      bin.activeReference += frames;
      lastReferenceActivity_ = receipt_qpc100ns;
    }
  }

  [[nodiscard]] ActivitySnapshot
  snapshot(std::uint64_t now_qpc100ns) const noexcept {
    ActivitySnapshot result;
    if (!now_qpc100ns) {
      return result;
    }
    SignalMoments microphone, output, reference, downmix;
    std::uint64_t changed = 0, usable = 0, subtracting = 0;
    std::uint64_t activeMicrophone = 0, activeReference = 0;
    const auto serial = now_qpc100ns / binTicks;
    for (const auto &bin : bins_) {
      if (!bin.occupied || bin.serial > serial ||
          serial - bin.serial >= bins_.size()) {
        continue;
      }
      accumulate(microphone, bin.microphone);
      accumulate(output, bin.output);
      accumulate(reference, bin.reference);
      accumulate(downmix, bin.downmix);
      result.mic_seconds += bin.micSeconds;
      result.reference_seconds += bin.referenceSeconds;
      changed += bin.changed;
      usable += bin.usable;
      subtracting += bin.subtracting;
      activeMicrophone += bin.activeMicrophone;
      activeReference += bin.activeReference;
    }
    result.mic_observed = microphone.samples != 0;
    result.reference_observed = reference.samples != 0;
    result.mic_rms = rms(microphone);
    result.mic_peak = microphone.peak;
    result.output_rms = rms(output);
    result.output_peak = output.peak;
    result.reference_rms = rms(reference);
    result.reference_peak = reference.peak;
    result.downmix_rms = rms(downmix);
    result.downmix_peak = downmix.peak;
    if (microphone.samples) {
      const auto denominator = static_cast<double>(microphone.samples);
      result.changed_fraction = static_cast<float>(changed / denominator);
      result.usable_fraction = static_cast<float>(usable / denominator);
      result.subtraction_fraction =
          static_cast<float>(subtracting / denominator);
      result.mic_active_fraction =
          static_cast<float>(activeMicrophone / denominator);
    }
    if (downmix.samples) {
      result.reference_active_fraction = static_cast<float>(
          static_cast<double>(activeReference) / downmix.samples);
    }
    result.reference_receipt_valid = age(now_qpc100ns, lastReferenceReceipt_,
                                         result.reference_receipt_age_ms);
    result.mic_activity_valid =
        age(now_qpc100ns, lastMicrophoneActivity_, result.mic_activity_age_ms);
    result.reference_activity_valid = age(now_qpc100ns, lastReferenceActivity_,
                                          result.reference_activity_age_ms);
    return result;
  }

private:
  static constexpr std::uint64_t binTicks = 500000;
  struct Bin {
    SignalMoments microphone, output, reference, downmix;
    double micSeconds{}, referenceSeconds{};
    std::uint64_t changed{}, usable{}, subtracting{}, serial{};
    std::uint64_t activeMicrophone{}, activeReference{};
    bool occupied{};
  };

  static bool valid(std::uint64_t timestamp, double rate, std::uint32_t frames,
                    const SignalMoments &moments) noexcept {
    return timestamp && std::isfinite(rate) && rate >= 8000 && rate <= 192000 &&
           frames && moments.samples && std::isfinite(moments.energy) &&
           moments.energy >= 0 && std::isfinite(moments.peak) &&
           moments.peak >= 0;
  }

  static bool active(const SignalMoments &value) noexcept {
    return value.energy / value.samples >
           static_cast<double>(activity_rms) * activity_rms;
  }

  static void accumulate(SignalMoments &destination,
                         const SignalMoments &source) noexcept {
    destination.energy += source.energy;
    destination.samples += source.samples;
    destination.peak = std::max(destination.peak, source.peak);
  }

  static float rms(const SignalMoments &value) noexcept {
    return value.samples
               ? static_cast<float>(std::sqrt(value.energy / value.samples))
               : 0;
  }

  static bool age(std::uint64_t now, std::uint64_t timestamp,
                  double &milliseconds) noexcept {
    if (!timestamp || now < timestamp) {
      return false;
    }
    milliseconds = (now - timestamp) / 10000.0;
    return true;
  }

  Bin &at(std::uint64_t timestamp) noexcept {
    const auto serial = timestamp / binTicks;
    auto &bin = bins_[serial % bins_.size()];
    if (!bin.occupied || bin.serial != serial) {
      bin = {};
      bin.serial = serial;
      bin.occupied = true;
    }
    return bin;
  }

  std::array<Bin, 60> bins_{};
  std::uint64_t lastMicrophoneReceipt_{}, lastReferenceReceipt_{};
  std::uint64_t lastMicrophoneActivity_{}, lastReferenceActivity_{};
};

} // namespace buncancel::plugin
