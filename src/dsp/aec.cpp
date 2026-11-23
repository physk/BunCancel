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

// Nearby samples belong to one finite echo-path hypothesis. A separated peak
// with comparable evidence means the source does not identify a unique delay.
void recordPeak(float score, int lag, float &bestScore, int &bestLag,
                float &secondScore, int &secondLag) noexcept {
  constexpr int neighborhood = 128;
  if (score > bestScore) {
    if (std::abs(lag - secondLag) <= neighborhood) {
      secondScore = 0;
    }
    if (std::abs(lag - bestLag) > neighborhood && bestScore > secondScore) {
      secondScore = bestScore;
      secondLag = bestLag;
    }
    bestScore = score;
    bestLag = lag;
  } else if (std::abs(lag - bestLag) > neighborhood && score > secondScore) {
    secondScore = score;
    secondLag = lag;
  }
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
  shadowWeights_.resize(taps_);
  reset();
}

void Aec::reset() noexcept {
  std::fill(reference_.begin(), reference_.end(), 0.0F);
  std::fill(microphone_.begin(), microphone_.end(), 0.0F);
  clearFilter();
  time_ = 0;
  scanEnd_ = 0;
  scanLag_ = 0;
  bestLag_ = secondLag_ = 0;
  delay_ = 0;
  strongCandidates_ = 0;
  diagnosticScanLag_ = -diagnosticLookback_;
  diagnosticBestLag_ = diagnosticSecondLag_ = 0;
  diagnosticStrongCandidates_ = 0;
  diagnosticBestScore_ = diagnosticSecondScore_ = 0;
  diagnosticScanEnd_ = 0;
  modelDiagnostics_ = {};
  excludedDiagnostics_ = {};
  lostSamples_ = 0;
  bestScore_ = secondScore_ = 0;
  confidence_ = 0;
  micPower_ = 0;
  predictedPower_ = 0;
  crossPower_ = 0;
  outputPower_ = 0;
  subtractionPower_ = 0;
  subtractionCrossPower_ = 0;
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
    bestScore_ = secondScore_ = 0;
    bestLag_ = secondLag_ = 0;
    strongCandidates_ = 0;
    if (modelDiagnostics_.state == AlignmentState::Warmup) {
      modelDiagnostics_.state = AlignmentState::Searching;
    }
  }
  const float score = correlation(scanLag_, scanEnd_);
  if (score > 0.65F) {
    ++strongCandidates_;
  }
  recordPeak(score, scanLag_, bestScore_, bestLag_, secondScore_, secondLag_);
  if (++scanLag_ <= maxDelay_) {
    return;
  }
  const float currentScore = aligned_ ? correlation(delay_, scanEnd_) : 0;
  modelDiagnostics_.peakLagSamples = bestLag_;
  modelDiagnostics_.peakCorrelation = bestScore_;
  modelDiagnostics_.peakValid = bestScore_ > 0;
  modelDiagnostics_.competingPeakCorrelation = secondScore_;
  const bool ambiguous =
      strongCandidates_ >= 2 * taps_ ||
      (secondScore_ > 0.65F && bestScore_ < secondScore_ + 0.08F);
  modelDiagnostics_.candidateValid =
      bestScore_ > 0.65F && !ambiguous && correlation(bestLag_, time_) > 0.65F;
  modelDiagnostics_.candidateLagSamples = bestLag_;
  modelDiagnostics_.state = AlignmentState::InsufficientCorrelation;
  if (bestScore_ > 0.65F && ambiguous) {
    modelDiagnostics_.state = AlignmentState::Ambiguous;
  } else if (modelDiagnostics_.candidateValid && bestLag_ <= 1) {
    modelDiagnostics_.state = AlignmentState::SelfMonitor;
  }
  // Require strong observed correlation before ever subtracting a signal.
  // Reacquisition is intentionally conservative during independent near speech.
  if (bestScore_ > 0.65F && !ambiguous && bestLag_ > 1 &&
      correlation(bestLag_, time_) > 0.65F &&
      (!aligned_ || bestScore_ > currentScore + 0.12F)) {
    if (!aligned_ || bestLag_ != delay_) {
      delay_ = bestLag_;
      clearFilter();
    }
    aligned_ = true;
  }
  confidence_ = aligned_ ? correlation(delay_, time_) : 0;
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
    diagnosticBestScore_ = diagnosticSecondScore_ = 0;
    diagnosticBestLag_ = diagnosticSecondLag_ = 0;
    diagnosticStrongCandidates_ = 0;
  }
  const float score = correlation(diagnosticScanLag_, diagnosticScanEnd_);
  diagnosticStrongCandidates_ += score > 0.65F;
  recordPeak(score, diagnosticScanLag_, diagnosticBestScore_,
             diagnosticBestLag_, diagnosticSecondScore_, diagnosticSecondLag_);
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
  excludedDiagnostics_.competingPeakCorrelation = diagnosticSecondScore_;
  excludedDiagnostics_.candidateLagSamples = diagnosticBestLag_;
  excludedDiagnostics_.candidateValid =
      diagnosticBestScore_ > 0.65F && diagnosticStrongCandidates_ < 2 * taps_ &&
      (diagnosticSecondScore_ <= 0.65F ||
       diagnosticBestScore_ >= diagnosticSecondScore_ + 0.08F) &&
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
    result.competingPeakCorrelation =
        excludedDiagnostics_.competingPeakCorrelation;
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
    if (lastProcessSubtractionPower_ > 1.0e-12 && confidence_ >= 0.2F &&
        subtractionPower_ > 1.0e-10 && subtractionCrossPower_ > 0 &&
        coherence > 0.25 && outputPower_ < micPower_ * 0.977 &&
        adaptedSamples_ >= 2048) {
      result.reductionDb = static_cast<float>(
          10 * std::log10(micPower_ / (outputPower_ + 1.0e-20)));
      result.reductionValid = true;
    }
  }
  result.adaptedSamples = adaptedSamples_;
  result.shadowAdaptedSamples = shadowAdaptedSamples_;
  result.shadowPromotions = shadowPromotions_;
  result.filterResetCount = filterResetCount_;
  result.samplesSinceReset = filterAgeSamples_;
  for (const auto weight : weights_) {
    result.weightsNorm += std::abs(weight);
  }
  result.predictionRms = static_cast<float>(std::sqrt(predictedPower_));
  result.outputRms = static_cast<float>(std::sqrt(outputPower_));
  result.microphoneRms = static_cast<float>(std::sqrt(micPower_));
  result.subtractionRms = static_cast<float>(std::sqrt(subtractionPower_));
  result.subtractionGain = subtractionGain_;
  result.doubleTalkFreezeRatio = static_cast<float>(doubleTalkFreezeRatio_);
  result.liveConfidence = confidence_;
  result.predictionCoherence = static_cast<float>(
      crossPower_ * crossPower_ / (micPower_ * predictedPower_ + 1.0e-20));
  result.subtractionActive =
      aligned_ && !missing_ && lastProcessSubtractionPower_ > 1.0e-12;
  result.shadowValidating = shadowPhaseSamples_ >= sampleRate_ / 10;
  if (missing_) {
    result.state = AlignmentState::ReferenceMissing;
  }
  return result;
}

void Aec::clearFilter() noexcept {
  std::fill(weights_.begin(), weights_.end(), 0.0F);
  std::fill(shadowWeights_.begin(), shadowWeights_.end(), 0.0F);
  adaptedSamples_ = shadowAdaptedSamples_ = 0;
  shadowPhaseSamples_ = shadowEligibleSamples_ = 0;
  shadowMicEnergy_ = shadowPredictionEnergy_ = shadowCrossEnergy_ = 0;
  shadowForegroundError_ = 0;
  predictedPower_ = crossPower_ = 0;
  subtractionPower_ = subtractionCrossPower_ = 0;
  subtractionGain_ = 0;
  doubleTalkFreezeRatio_ = 0;
  filterAgeSamples_ = 0;
  lastProcessSubtractionPower_ = 0;
  ++filterResetCount_;
}

void Aec::updateShadow(float microphone, float energy,
                       float foregroundOutput) noexcept {
  const int trainingSamples = sampleRate_ / 10;
  const int validationSamples = sampleRate_ / 20;
  float shadowPrediction = 0;
  for (int k = 0; k < taps_; ++k) {
    shadowPrediction += shadowWeights_[static_cast<std::size_t>(k)] *
                        tapValues_[static_cast<std::size_t>(k)];
  }
  const bool eligible =
      microphone != 0 && confidence_ >= 0.65F && energy > 1.0e-8F;
  if (shadowPhaseSamples_ < trainingSamples) {
    if (eligible) {
      // This unrendered model can continue learning a path whose foreground
      // coherence froze before convergence. The relative normalization makes
      // its learning rate useful for quiet references. Validation on later,
      // frozen-weight samples decides whether it can reach the output.
      const float step =
          0.1F * (microphone - shadowPrediction) / (energy * 1.001F + 1.0e-6F);
      float norm = 0;
      for (int k = 0; k < taps_; ++k) {
        auto &weight = shadowWeights_[static_cast<std::size_t>(k)];
        weight =
            std::clamp(weight + step * tapValues_[static_cast<std::size_t>(k)],
                       -4.0F, 4.0F);
        norm += std::abs(weight);
      }
      if (norm > 4) {
        for (auto &weight : shadowWeights_) {
          weight *= 4 / norm;
        }
      }
      ++shadowAdaptedSamples_;
    }
  } else {
    shadowMicEnergy_ += static_cast<double>(microphone) * microphone;
    shadowPredictionEnergy_ +=
        static_cast<double>(shadowPrediction) * shadowPrediction;
    shadowCrossEnergy_ += static_cast<double>(microphone) * shadowPrediction;
    shadowForegroundError_ +=
        static_cast<double>(foregroundOutput) * foregroundOutput;
    shadowEligibleSamples_ += eligible;
  }
  if (++shadowPhaseSamples_ < trainingSamples + validationSamples) {
    return;
  }
  const double gain = std::clamp(
      shadowCrossEnergy_ / (shadowPredictionEnergy_ + 1.0e-20), 0.0, 1.0);
  const double candidateError = shadowMicEnergy_ -
                                2 * gain * shadowCrossEnergy_ +
                                gain * gain * shadowPredictionEnergy_;
  const double coherence =
      shadowCrossEnergy_ * shadowCrossEnergy_ /
      (shadowMicEnergy_ * shadowPredictionEnergy_ + 1.0e-20);
  if (shadowEligibleSamples_ >= validationSamples * 9 / 10 &&
      shadowAdaptedSamples_ >=
          static_cast<std::uint64_t>(trainingSamples / 2) &&
      shadowCrossEnergy_ > 0 && coherence > 0.5 &&
      candidateError < shadowMicEnergy_ * 0.8 &&
      candidateError < shadowForegroundError_ * 0.9) {
    for (int k = 0; k < taps_; ++k) {
      weights_[static_cast<std::size_t>(k)] =
          static_cast<float>(gain) *
          shadowWeights_[static_cast<std::size_t>(k)];
    }
    adaptedSamples_ = std::max(adaptedSamples_, shadowAdaptedSamples_);
    ++shadowPromotions_;
  }
  shadowPhaseSamples_ = shadowEligibleSamples_ = 0;
  shadowMicEnergy_ = shadowPredictionEnergy_ = shadowCrossEnergy_ = 0;
  shadowForegroundError_ = 0;
}

void Aec::process(const float *mic, const float *reference, float *out,
                  int count) noexcept {
  if (count <= 0 || mic == nullptr || out == nullptr) {
    return;
  }
  lastProcessSubtractionPower_ = 0;
  if (reference == nullptr) {
    if (!missing_) {
      reset();
      missing_ = true;
    }
    for (int i = 0; i < count; ++i) {
      out[i] = bounded(mic[i]);
    }
    filterAgeSamples_ += static_cast<std::uint64_t>(count);
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
        clearFilter();
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
    doubleTalkFreezeRatio_ = smoothing * doubleTalkFreezeRatio_ +
                             (1 - smoothing) * (aligned_ && doubleTalk ? 1 : 0);
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
    subtractionGain_ = static_cast<float>(
        std::clamp(crossPower_ / (predictedPower_ + 1.0e-12), 0.0, 1.0));
    // A stale model must not amplify interference. Constrained regression
    // decreases cancellation when its prediction ceases to explain the mic.
    // An unflagged digital microphone mute must not turn the learned echo
    // model into an audio source. Exact zero needs no level threshold that
    // could gate quiet near-end speech.
    out[i] = m == 0 ? 0 : bounded(m - subtractionGain_ * prediction);
    const double subtraction = static_cast<double>(m) - out[i];
    lastProcessSubtractionPower_ += subtraction * subtraction;
    outputPower_ = smoothing * outputPower_ + (1 - smoothing) * out[i] * out[i];
    subtractionPower_ = smoothing * subtractionPower_ +
                        (1 - smoothing) * subtraction * subtraction;
    subtractionCrossPower_ =
        smoothing * subtractionCrossPower_ + (1 - smoothing) * m * subtraction;
    if (aligned_) {
      updateShadow(m, energy, out[i]);
    }
    ++filterAgeSamples_;
    ++time_;
  }
  lastProcessSubtractionPower_ /= count;
}

} // namespace buncancel
