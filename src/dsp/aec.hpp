#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace buncancel {

// Original mono baseline. Construction allocates; process and reset do not.
// The caller supplies reference samples on the microphone clock. Positive delay
// means playback precedes its arrival at the microphone. No lookahead is used.
// This class is single-thread owned, including its diagnostic accessors.
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
  static constexpr int correlationStride_ = 4;
  static constexpr int correlationSpan_ =
      correlationPoints_ * correlationStride_;
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
