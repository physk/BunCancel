#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace buncancel {

// Original mono baseline. Construction allocates; process and reset do not.
// The caller supplies reference samples on the microphone clock. Positive delay
// means playback precedes its arrival at the microphone. No lookahead is used.
// This class is single-thread owned, including its diagnostic accessors.
// Scope: <= 333 ms delay, 128-tap tail, mono reference already on the mic
// clock. Periodic ambiguity and direct self-monitoring degrade to dry
// microphone. Delayed near-end contamination cannot be identified reliably from
// two signals; the capture/routing layer must keep the microphone out of the
// reference.
class Aec {
public:
  enum class AlignmentState {
    Warmup,
    Searching,
    InsufficientCorrelation,
    Ambiguous,
    SelfMonitor,
    DelayOutOfRange,
    Aligned,
    ReferenceMissing
  };
  struct Diagnostics {
    AlignmentState state = AlignmentState::Warmup;
    // Latest completed search peaks include weak matches. candidateValid
    // additionally requires a strong, unambiguous match on newer audio.
    // Signed lags use the same reference/microphone clock as process().
    int peakLagSamples = 0;
    float peakCorrelation = 0;
    bool peakValid = false;
    float competingPeakCorrelation = 0;
    int candidateLagSamples = 0;
    bool candidateValid = false;
    // Rolling mic/output power reduction with coherent subtraction evidence.
    // This is not ERLE and cannot distinguish near-end speech from echo.
    float reductionDb = 0;
    bool reductionValid = false;
    // Foreground updates include samples learned by a promoted shadow model.
    // Counters and RMS describe model activity, not far-only echo quality.
    std::uint64_t adaptedSamples = 0;
    std::uint64_t shadowAdaptedSamples = 0;
    std::uint64_t shadowPromotions = 0;
    std::uint64_t filterResetCount = 0;
    // Actual processed samples since the most recent filter invalidation.
    std::uint64_t samplesSinceReset = 0;
    float weightsNorm = 0;
    float predictionRms = 0;
    float outputRms = 0;
    float microphoneRms = 0;
    float subtractionRms = 0;
    float subtractionGain = 0;
    float doubleTalkFreezeRatio = 0;
    float liveConfidence = 0;
    float predictionCoherence = 0;
    // Uses applied subtraction in the last completed process call, including
    // blocks whose final sample is an exact zero. A wholly silent call is
    // false.
    bool subtractionActive = false;
    bool shadowValidating = false;
  };
  explicit Aec(int sampleRate);
  void process(const float *mic, const float *reference, float *out,
               int count) noexcept;
  void reset() noexcept;
  [[nodiscard]] int delaySamples() const noexcept { return delay_; }
  [[nodiscard]] bool aligned() const noexcept { return aligned_; }
  [[nodiscard]] float alignmentConfidence() const noexcept {
    return confidence_;
  }
  [[nodiscard]] int latencySamples() const noexcept { return 0; }
  [[nodiscard]] int maxDelaySamples() const noexcept { return maxDelay_; }
  // Diagnostic-only excluded-delay search covers [-40 ms, 0) and
  // (maxDelaySamples(), 500 ms]. It never changes the cancellation lag.
  [[nodiscard]] Diagnostics diagnostics() const noexcept;

private:
  static constexpr int taps_ = 128;
  static constexpr int correlationPoints_ = 128;
  int correlationSpan_ = 0;
  std::array<int, correlationPoints_> correlationOffsets_{};
  float history(const std::vector<float> &values,
                std::uint64_t time) const noexcept;
  float correlation(int lag, std::uint64_t end) const noexcept;
  void searchDelay() noexcept;
  void searchExcludedDelay() noexcept;
  void clearFilter() noexcept;
  void updateShadow(float microphone, float energy,
                    float foregroundOutput) noexcept;

  std::size_t historyMask_ = 0;
  int sampleRate_;
  int maxDelay_;
  int diagnosticMaxDelay_;
  int diagnosticLookback_;
  std::vector<float> reference_;
  std::vector<float> microphone_;
  std::vector<float> weights_;
  std::vector<float> tapValues_;
  std::vector<float> shadowWeights_;
  std::uint64_t time_ = 0;
  std::uint64_t scanEnd_ = 0;
  int scanLag_ = 0;
  int bestLag_ = 0;
  int secondLag_ = 0;
  int delay_ = 0;
  int strongCandidates_ = 0;
  int diagnosticScanLag_ = 0;
  int diagnosticBestLag_ = 0;
  int diagnosticSecondLag_ = 0;
  int diagnosticStrongCandidates_ = 0;
  int lostSamples_ = 0;
  float bestScore_ = 0;
  float secondScore_ = 0;
  float confidence_ = 0;
  float diagnosticBestScore_ = 0;
  float diagnosticSecondScore_ = 0;
  std::uint64_t diagnosticScanEnd_ = 0;
  Diagnostics modelDiagnostics_;
  Diagnostics excludedDiagnostics_;
  double outputPower_ = 0;
  double subtractionPower_ = 0;
  double subtractionCrossPower_ = 0;
  double micPower_ = 0;
  double predictedPower_ = 0;
  double crossPower_ = 0;
  std::uint64_t adaptedSamples_ = 0;
  std::uint64_t shadowAdaptedSamples_ = 0;
  std::uint64_t shadowPromotions_ = 0;
  std::uint64_t filterResetCount_ = 0;
  std::uint64_t filterAgeSamples_ = 0;
  int shadowPhaseSamples_ = 0;
  int shadowEligibleSamples_ = 0;
  double shadowMicEnergy_ = 0;
  double shadowPredictionEnergy_ = 0;
  double shadowCrossEnergy_ = 0;
  double shadowForegroundError_ = 0;
  double doubleTalkFreezeRatio_ = 0;
  double lastProcessSubtractionPower_ = 0;
  float subtractionGain_ = 0;
  bool aligned_ = false;
  bool missing_ = false;
};

} // namespace buncancel
