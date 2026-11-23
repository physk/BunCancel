#include "aec.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace buncancel {
namespace {
float bounded(float value) noexcept {
  return std::isfinite(value) ? std::clamp(value, -16.0F, 16.0F) : 0.0F;
}
} // namespace

Aec::Aec(int sampleRate) : sampleRate_(sampleRate), maxDelay_(sampleRate / 3) {
  if (sampleRate < 8000 || sampleRate > 192000) {
    throw std::invalid_argument(
        "AEC sample rate must be between 8000 and 192000 Hz");
  }
  const auto size = std::bit_ceil(
      static_cast<std::size_t>(3 * maxDelay_ + correlationSpan_ + taps_ + 8));
  historyMask_ = size - 1;
  reference_.resize(size);
  microphone_.resize(size);
  weights_.resize(taps_);
  tapValues_.resize(taps_);
}

void Aec::reset() noexcept {
  std::fill(reference_.begin(), reference_.end(), 0.0F);
  std::fill(microphone_.begin(), microphone_.end(), 0.0F);
  std::fill(weights_.begin(), weights_.end(), 0.0F);
  time_ = 0;
  scanEnd_ = 0;
  scanLag_ = 0;
  bestLag_ = 0;
  delay_ = 0;
  strongCandidates_ = 0;
  lostSamples_ = 0;
  bestScore_ = 0;
  confidence_ = 0;
  micPower_ = 0;
  predictedPower_ = 0;
  crossPower_ = 0;
  adaptedSamples_ = 0;
  aligned_ = false;
  missing_ = false;
}

float Aec::history(const std::vector<float> &values,
                   std::uint64_t time) const noexcept {
  return values[static_cast<std::size_t>(time & historyMask_)];
}

float Aec::correlation(int lag, std::uint64_t end) const noexcept {
  double cross = 0;
  double micEnergy = 0;
  double refEnergy = 0;
  for (int i = 0; i < correlationPoints_; ++i) {
    const auto t = end - static_cast<std::uint64_t>(i * correlationStride_);
    // First differences reduce low-frequency periodic ambiguity. This is
    // evidence for alignment only, not a modification of the output signal.
    const double m = history(microphone_, t) - history(microphone_, t - 1);
    const double r =
        history(reference_, t - lag) - history(reference_, t - lag - 1);
    cross += m * r;
    micEnergy += m * m;
    refEnergy += r * r;
  }
  if (micEnergy < 1.0e-10 || refEnergy < 1.0e-10) {
    return 0;
  }
  return static_cast<float>(std::abs(cross) / std::sqrt(micEnergy * refEnergy));
}

void Aec::searchDelay() noexcept {
  if (time_ < static_cast<std::uint64_t>(maxDelay_ + correlationSpan_ + 1)) {
    return;
  }
  if (scanLag_ == 0) {
    scanEnd_ = time_;
    bestScore_ = 0;
    bestLag_ = 0;
    strongCandidates_ = 0;
  }
  const float score = correlation(scanLag_, scanEnd_);
  if (score > 0.65F) {
    ++strongCandidates_;
  }
  if (score > bestScore_) {
    bestScore_ = score;
    bestLag_ = scanLag_;
  }
  if (++scanLag_ <= maxDelay_) {
    return;
  }
  const float currentScore = aligned_ ? correlation(delay_, scanEnd_) : 0;
  // Require strong observed correlation before ever subtracting a signal.
  // Reacquisition is intentionally conservative during independent near speech.
  if (bestScore_ > 0.65F && strongCandidates_ < 2 * taps_ && bestLag_ > 1 &&
      correlation(bestLag_, time_) > 0.65F &&
      (!aligned_ || bestScore_ > currentScore + 0.12F)) {
    if (!aligned_ || bestLag_ != delay_) {
      delay_ = bestLag_;
      std::fill(weights_.begin(), weights_.end(), 0.0F);
      adaptedSamples_ = 0;
      predictedPower_ = 0;
      crossPower_ = 0;
    }
    aligned_ = true;
  }
  confidence_ = aligned_ ? correlation(delay_, scanEnd_) : bestScore_;
  scanLag_ = 0;
}

void Aec::process(const float *mic, const float *reference, float *out,
                  int count) noexcept {
  if (count <= 0 || mic == nullptr || out == nullptr) {
    return;
  }
  if (reference == nullptr) {
    if (!missing_) {
      reset();
      missing_ = true;
    }
    for (int i = 0; i < count; ++i) {
      out[i] = bounded(mic[i]);
    }
    return;
  }
  missing_ = false;
  for (int i = 0; i < count; ++i) {
    const float m = bounded(mic[i]);
    const float r = bounded(reference[i]);
    reference_[static_cast<std::size_t>(time_ & historyMask_)] = r;
    microphone_[static_cast<std::size_t>(time_ & historyMask_)] = m;
    searchDelay();
    if (aligned_ && (time_ & 255U) == 0) {
      confidence_ = correlation(delay_, time_);
      lostSamples_ = confidence_ < 0.2F ? lostSamples_ + 256 : 0;
      if (lostSamples_ >= sampleRate_ / 10) {
        aligned_ = false;
        std::fill(weights_.begin(), weights_.end(), 0.0F);
        adaptedSamples_ = 0;
        lostSamples_ = 0;
      }
    }
    float prediction = 0;
    float energy = 0;
    if (aligned_) {
      for (int k = 0; k < taps_; ++k) {
        const auto offset = static_cast<std::uint64_t>(delay_ + k);
        const float x =
            time_ >= offset ? history(reference_, time_ - offset) : 0;
        tapValues_[static_cast<std::size_t>(k)] = x;
        prediction += weights_[static_cast<std::size_t>(k)] * x;
        energy += x * x;
      }
    }
    const float error = m - prediction;
    constexpr double smoothing = 0.995;
    micPower_ = smoothing * micPower_ + (1 - smoothing) * m * m;
    predictedPower_ =
        smoothing * predictedPower_ + (1 - smoothing) * prediction * prediction;
    crossPower_ = smoothing * crossPower_ + (1 - smoothing) * m * prediction;
    const double coherence =
        crossPower_ * crossPower_ / (micPower_ * predictedPower_ + 1.0e-20);
    const bool warming =
        adaptedSamples_ < static_cast<std::uint64_t>(sampleRate_ / 10);
    const bool doubleTalk = !warming && coherence < 0.8;
    if (aligned_ && energy > 1.0e-8F && !doubleTalk) {
      const float step = 0.15F * error / (energy + 1.0e-6F);
      for (int k = 0; k < taps_; ++k) {
        auto &w = weights_[static_cast<std::size_t>(k)];
        w = std::clamp(w + step * tapValues_[static_cast<std::size_t>(k)],
                       -4.0F, 4.0F);
        if (std::abs(w) < 1.0e-20F) {
          w = 0;
        }
      }
      ++adaptedSamples_;
    }
    out[i] = bounded(error);
    ++time_;
  }
}

} // namespace buncancel
