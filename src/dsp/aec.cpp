#include "aec.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace buncancel {
namespace {
float bounded(float value) noexcept {
  if (!std::isfinite(value) || std::abs(value) < 1.0e-20F) {
    return 0;
  }
  return std::clamp(value, -16.0F, 16.0F);
}
} // namespace

Aec::Aec(int sampleRate)
    : sampleRate_(sampleRate), maxDelay_(sampleRate / 3),
      diagnosticMaxDelay_(sampleRate / 2),
      diagnosticLookback_(sampleRate / 25) {
  if (sampleRate < 8000 || sampleRate > 192000) {
    throw std::invalid_argument(
        "AEC sample rate must be between 8000 and 192000 Hz");
  }
  correlationSpan_ = sampleRate_ / 5;
  // Sparse, nonuniform observations across 200 ms retain a fixed cost while
  // distinguishing changing harmonic signals from a repeated pure tone.
  std::uint32_t seed = 101;
  for (int i = 0; i < correlationPoints_; ++i) {
    seed = seed * 1664525U + 1013904223U;
    const int width = correlationSpan_ / correlationPoints_;
    correlationOffsets_[static_cast<std::size_t>(i)] =
        i * width + static_cast<int>(seed % static_cast<unsigned>(width));
  }
  const auto size = std::bit_ceil(
      static_cast<std::size_t>(3 * maxDelay_ + correlationSpan_ + taps_ + 8));
  historyMask_ = size - 1;
  reference_.resize(size);
  microphone_.resize(size);
  weights_.resize(taps_);
  tapValues_.resize(taps_);
  reset();
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
  diagnosticScanLag_ = -diagnosticLookback_;
  diagnosticBestLag_ = 0;
  diagnosticStrongCandidates_ = 0;
  diagnosticBestScore_ = 0;
  diagnosticScanEnd_ = 0;
  modelDiagnostics_ = {};
  excludedDiagnostics_ = {};
  lostSamples_ = 0;
  bestScore_ = 0;
  confidence_ = 0;
  micPower_ = 0;
  predictedPower_ = 0;
  crossPower_ = 0;
  outputPower_ = 0;
  subtractionPower_ = 0;
  subtractionCrossPower_ = 0;
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
    const auto t = end - static_cast<std::uint64_t>(
                             correlationOffsets_[static_cast<std::size_t>(i)]);
    // First differences reduce low-frequency periodic ambiguity. This is
    // evidence for alignment only, not a modification of the output signal.
    const double m = history(microphone_, t) - history(microphone_, t - 1);
    const auto referenceTime = lag >= 0 ? t - static_cast<unsigned>(lag)
                                        : t + static_cast<unsigned>(-lag);
    const double r = history(reference_, referenceTime) -
                     history(reference_, referenceTime - 1);
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
    if (modelDiagnostics_.state == AlignmentState::Warmup) {
      modelDiagnostics_.state = AlignmentState::Searching;
    }
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
  modelDiagnostics_.peakLagSamples = bestLag_;
  modelDiagnostics_.peakCorrelation = bestScore_;
  modelDiagnostics_.peakValid = bestScore_ > 0;
  modelDiagnostics_.candidateValid = bestScore_ > 0.65F &&
                                     strongCandidates_ < 2 * taps_ &&
                                     correlation(bestLag_, time_) > 0.65F;
  modelDiagnostics_.candidateLagSamples = bestLag_;
  modelDiagnostics_.state = AlignmentState::InsufficientCorrelation;
  if (bestScore_ > 0.65F && strongCandidates_ >= 2 * taps_) {
    modelDiagnostics_.state = AlignmentState::Ambiguous;
  } else if (modelDiagnostics_.candidateValid && bestLag_ <= 1) {
    modelDiagnostics_.state = AlignmentState::SelfMonitor;
  }
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
  confidence_ = aligned_ ? correlation(delay_, scanEnd_) : 0;
  scanLag_ = 0;
}

void Aec::searchExcludedDelay() noexcept {
  // This independent scan never supplies the cancellation filter. Its older
  // microphone endpoint permits negative lag observations without lookahead.
  if (time_ <
      static_cast<std::uint64_t>(diagnosticMaxDelay_ + correlationSpan_ +
                                 diagnosticLookback_ + 1)) {
    return;
  }
  if (diagnosticScanLag_ == -diagnosticLookback_) {
    diagnosticScanEnd_ = time_ - diagnosticLookback_;
    diagnosticBestScore_ = 0;
    diagnosticBestLag_ = 0;
    diagnosticStrongCandidates_ = 0;
  }
  const float score = correlation(diagnosticScanLag_, diagnosticScanEnd_);
  diagnosticStrongCandidates_ += score > 0.65F;
  if (score > diagnosticBestScore_) {
    diagnosticBestScore_ = score;
    diagnosticBestLag_ = diagnosticScanLag_;
  }
  ++diagnosticScanLag_;
  if (diagnosticScanLag_ == 0) {
    diagnosticScanLag_ = maxDelay_ + 1;
  }
  if (diagnosticScanLag_ <= diagnosticMaxDelay_) {
    return;
  }
  excludedDiagnostics_.peakLagSamples = diagnosticBestLag_;
  excludedDiagnostics_.peakCorrelation = diagnosticBestScore_;
  excludedDiagnostics_.peakValid = diagnosticBestScore_ > 0;
  excludedDiagnostics_.candidateLagSamples = diagnosticBestLag_;
  excludedDiagnostics_.candidateValid =
      diagnosticBestScore_ > 0.65F && diagnosticStrongCandidates_ < 2 * taps_ &&
      correlation(diagnosticBestLag_, time_ - diagnosticLookback_) > 0.65F;
  diagnosticScanLag_ = -diagnosticLookback_;
}

Aec::Diagnostics Aec::diagnostics() const noexcept {
  auto result = modelDiagnostics_;
  if (excludedDiagnostics_.peakValid &&
      excludedDiagnostics_.peakCorrelation > result.peakCorrelation) {
    result.peakLagSamples = excludedDiagnostics_.peakLagSamples;
    result.peakCorrelation = excludedDiagnostics_.peakCorrelation;
    result.peakValid = true;
  }
  if (excludedDiagnostics_.candidateValid &&
      !modelDiagnostics_.candidateValid &&
      modelDiagnostics_.state != AlignmentState::Ambiguous) {
    result.state = AlignmentState::DelayOutOfRange;
    result.candidateLagSamples = excludedDiagnostics_.candidateLagSamples;
    result.candidateValid = true;
  }
  if (aligned_) {
    result.state = AlignmentState::Aligned;
    result.candidateLagSamples = delay_;
    result.candidateValid = true;
    const double coherence = subtractionCrossPower_ * subtractionCrossPower_ /
                             (micPower_ * subtractionPower_ + 1.0e-20);
    if (history(microphone_, time_ - 1) != 0 && confidence_ >= 0.2F &&
        subtractionPower_ > 1.0e-10 && subtractionCrossPower_ > 0 &&
        coherence > 0.25 && outputPower_ < micPower_ * 0.977 &&
        adaptedSamples_ >= 2048) {
      result.reductionDb = static_cast<float>(
          10 * std::log10(micPower_ / (outputPower_ + 1.0e-20)));
      result.reductionValid = true;
    }
  }
  if (missing_) {
    result.state = AlignmentState::ReferenceMissing;
  }
  return result;
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
    searchExcludedDelay();
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
    const double smoothing = 1.0 - 1.0 / (0.02 * sampleRate_);
    micPower_ = smoothing * micPower_ + (1 - smoothing) * m * m;
    predictedPower_ =
        smoothing * predictedPower_ + (1 - smoothing) * prediction * prediction;
    crossPower_ = smoothing * crossPower_ + (1 - smoothing) * m * prediction;
    const double coherence =
        crossPower_ * crossPower_ / (micPower_ * predictedPower_ + 1.0e-20);
    const bool warming =
        adaptedSamples_ <
        static_cast<std::uint64_t>(std::max(sampleRate_ / 10, 2048));
    const bool doubleTalk = warming ? confidence_ < 0.65F : coherence < 0.98;
    if (aligned_ && m != 0 && energy > 1.0e-8F && !doubleTalk) {
      // A fixed regularizer limits corrections in reference envelope gaps,
      // where unmodeled long-tail echo otherwise drives huge coefficients.
      const float step = 0.15F * error / (energy + 0.01F);
      for (int k = 0; k < taps_; ++k) {
        auto &w = weights_[static_cast<std::size_t>(k)];
        w = std::clamp(w + step * tapValues_[static_cast<std::size_t>(k)],
                       -4.0F, 4.0F);
        if (std::abs(w) < 1.0e-20F) {
          w = 0;
        }
      }
      float norm = 0;
      for (const auto weight : weights_) {
        norm += std::abs(weight);
      }
      if (norm > 4.0F) {
        // Bound aggregate gain, rather than only bounding each coefficient.
        const float scale = 4.0F / norm;
        for (auto &weight : weights_) {
          weight *= scale;
        }
      }
      ++adaptedSamples_;
    }
    const float subtractionGain = static_cast<float>(
        std::clamp(crossPower_ / (predictedPower_ + 1.0e-12), 0.0, 1.0));
    // A stale model must not amplify interference. Constrained regression
    // decreases cancellation when its prediction ceases to explain the mic.
    // An unflagged digital microphone mute must not turn the learned echo
    // model into an audio source. Exact zero needs no level threshold that
    // could gate quiet near-end speech.
    out[i] = m == 0 ? 0 : bounded(m - subtractionGain * prediction);
    const double subtraction = static_cast<double>(m) - out[i];
    outputPower_ = smoothing * outputPower_ + (1 - smoothing) * out[i] * out[i];
    subtractionPower_ = smoothing * subtractionPower_ +
                        (1 - smoothing) * subtraction * subtraction;
    subtractionCrossPower_ =
        smoothing * subtractionCrossPower_ + (1 - smoothing) * m * subtraction;
    ++time_;
  }
}

} // namespace buncancel
