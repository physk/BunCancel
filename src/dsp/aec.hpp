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

private:
  static constexpr int taps_ = 128;
  static constexpr int correlationPoints_ = 128;
  int correlationSpan_ = 0;
  std::array<int, correlationPoints_> correlationOffsets_{};
  float history(const std::vector<float> &values,
                std::uint64_t time) const noexcept;
  float correlation(int lag, std::uint64_t end) const noexcept;
  void searchDelay() noexcept;

  std::size_t historyMask_ = 0;
  int sampleRate_;
  int maxDelay_;
  std::vector<float> reference_;
  std::vector<float> microphone_;
  std::vector<float> weights_;
  std::vector<float> tapValues_;
  std::uint64_t time_ = 0;
  std::uint64_t scanEnd_ = 0;
  int scanLag_ = 0;
  int bestLag_ = 0;
  int delay_ = 0;
  int strongCandidates_ = 0;
  int lostSamples_ = 0;
  float bestScore_ = 0;
  float confidence_ = 0;
  double micPower_ = 0;
  double predictedPower_ = 0;
  double crossPower_ = 0;
  std::uint64_t adaptedSamples_ = 0;
  bool aligned_ = false;
  bool missing_ = false;
};

} // namespace buncancel
