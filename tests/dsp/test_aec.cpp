#include "aec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
bool countAllocations = false;
std::size_t allocations = 0;
} // namespace

void *operator new(std::size_t size) {
  if (countAllocations) {
    ++allocations;
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void operator delete(void *memory) noexcept {
  std::free(memory);
}
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::vector<float> noise(int count, std::uint32_t seed) {
  std::vector<float> result(static_cast<std::size_t>(count));
  for (auto &value : result) {
    seed = seed * 1664525U + 1013904223U;
    value = (static_cast<float>(seed >> 8) / 16777216.0F - 0.5F) * 0.5F;
  }
  return result;
}

double energy(const std::vector<float> &signal, int start, int end) {
  double result = 0;
  for (int i = start; i < end; ++i) {
    result += static_cast<double>(signal[i]) * signal[i];
  }
  return result;
}

std::vector<float> run(const std::vector<float> &mic,
                       const std::vector<float> &reference, int block,
                       int delay) {
  buncancel::Aec engine(48000);
  std::vector<float> output(mic.size());
  for (std::size_t i = 0; i < mic.size();
       i += static_cast<std::size_t>(block)) {
    engine.process(mic.data() + i, reference.data() + i, output.data() + i,
                   static_cast<int>(std::min(static_cast<std::size_t>(block),
                                             mic.size() - i)));
  }
  require(engine.aligned(), "failed to align broadband reference");
  require(engine.delaySamples() == delay, "incorrect delay estimate");
  return output;
}

void cancellationAndBlocks() {
  constexpr int rate = 48000;
  constexpr int count = rate * 4;
  constexpr int delay = 731;
  const auto reference = noise(count, 17);
  std::vector<float> mic(count);
  for (int i = delay + 23; i < count; ++i) {
    const float gain = i < rate * 2 ? 0.6F : 0.9F;
    mic[i] = gain * reference[i - delay] + 0.12F * reference[i - delay - 23];
  }
  const auto baseline = run(mic, reference, 64, delay);
  require(energy(baseline, rate, rate * 2) < energy(mic, rate, rate * 2) * 0.01,
          "single-talk echo did not converge");
  require(energy(baseline, rate * 3, count) <
              energy(mic, rate * 3, count) * 0.01,
          "gain change did not reconverge");
  for (const int block : {1, 127, 512, 2048}) {
    require(run(mic, reference, block, delay) == baseline,
            "block partition changes output");
  }
}

void doubleTalk() {
  constexpr int rate = 48000;
  constexpr int count = rate * 4;
  constexpr int delay = 211;
  const auto reference = noise(count, 19);
  const auto near = noise(count, 29);
  std::vector<float> mic(count);
  for (int i = delay; i < count; ++i) {
    mic[i] = 0.6F * reference[i - delay];
    if (i >= rate * 2) {
      mic[i] += near[i];
    }
  }
  const auto output = run(mic, reference, 97, delay);
  double distortion = 0;
  double cross = 0;
  for (int i = rate * 2; i < count; ++i) {
    const double error = output[i] - near[i];
    distortion += error * error;
    cross += static_cast<double>(output[i]) * near[i];
  }
  const double nearEnergy = energy(near, rate * 2, count);
  require(distortion < nearEnergy * 0.025,
          "double talk damaged near-end signal");
  require(cross / nearEnergy > 0.98, "double talk attenuated near-end signal");
}

void longDelayAndReferenceLoss() {
  constexpr int rate = 48000;
  constexpr int delay = 14000;
  const auto reference = noise(rate * 2, 41);
  std::vector<float> mic(reference.size());
  std::vector<float> output(reference.size());
  for (std::size_t i = delay; i < mic.size(); ++i) {
    mic[i] = reference[i - delay] * -0.7F;
  }
  buncancel::Aec engine(rate);
  engine.process(mic.data(), reference.data(), output.data(),
                 static_cast<int>(mic.size()));
  require(engine.aligned() && engine.delaySamples() == delay,
          "long delay alignment failed");
  require(energy(output, rate, rate * 2) < energy(mic, rate, rate * 2) * 0.01,
          "long delay cancellation failed");
  engine.process(mic.data(), nullptr, output.data(),
                 static_cast<int>(mic.size()));
  require(output == mic, "reference loss must be dry");
  require(!engine.aligned(), "reference loss retained stale alignment");
}

void safety() {
  buncancel::Aec engine(48000);
  auto mic = noise(2048, 43);
  std::vector<float> silent(mic.size());
  std::vector<float> output(mic.size());
  engine.process(mic.data(), silent.data(), output.data(),
                 static_cast<int>(mic.size()));
  require(output == mic, "silent reference modified microphone");
  mic[0] = std::numeric_limits<float>::quiet_NaN();
  mic[1] = std::numeric_limits<float>::infinity();
  silent[2] = -std::numeric_limits<float>::infinity();
  mic[3] = std::numeric_limits<float>::max();
  engine.process(mic.data(), silent.data(), output.data(),
                 static_cast<int>(mic.size()));
  for (const auto value : output) {
    require(std::isfinite(value) && std::abs(value) <= 16.0F,
            "unsafe numeric output");
  }
  require(engine.latencySamples() == 0, "baseline unexpectedly adds lookahead");
  engine.process(nullptr, nullptr, nullptr, 0);
  bool rejected = false;
  try {
    buncancel::Aec invalid(0);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "invalid sample rate accepted");
}

void unusableAlignment() {
  constexpr int rate = 48000;
  constexpr int count = rate * 2;
  const auto reference = noise(count, 71);
  auto mic = noise(count, 73);
  std::vector<float> output(count);
  for (int i = 300; i < count; ++i) {
    mic[i] = reference[i - 300] * 0.6F;
  }
  buncancel::Aec engine(rate);
  engine.process(mic.data(), reference.data(), output.data(), count);
  require(engine.aligned(), "loss test never acquired alignment");
  const auto unrelated = noise(count, 79);
  engine.process(mic.data(), unrelated.data(), output.data(), count);
  require(!engine.aligned(), "unrelated reference retained stale filter");
  require(std::equal(output.begin() + rate, output.end(), mic.begin() + rate),
          "unusable alignment failed to recover dry microphone");

  engine.reset();
  engine.process(reference.data(), reference.data(), output.data(), count);
  require(!engine.aligned(), "direct microphone monitoring was accepted");
  require(output == reference, "direct microphone monitoring cancelled voice");

  std::vector<float> tone(count);
  for (int i = 0; i < count; ++i) {
    tone[i] =
        static_cast<float>(0.2 * std::sin(2 * 3.141592653589793 * i / 96));
    mic[i] = i > 300 ? tone[i - 300] * 0.7F : 0;
  }
  engine.reset();
  engine.process(mic.data(), tone.data(), output.data(), count);
  require(!engine.aligned(), "periodic ambiguity produced false alignment");
  require(output == mic, "ambiguous reference modified microphone");
}

void sampleRatesAndAllocation() {
  for (const int rate : {8000, 16000, 44100, 96000, 192000}) {
    const int count = rate * 2;
    const int delay = rate / 8;
    const auto reference = noise(count, 83);
    std::vector<float> mic(count);
    std::vector<float> output(count);
    for (int i = delay; i < count; ++i) {
      mic[i] = reference[i - delay] * 0.5F;
    }
    buncancel::Aec engine(rate);
    allocations = 0;
    countAllocations = true;
    engine.process(mic.data(), reference.data(), output.data(), count);
    const auto diagnostics = engine.diagnostics();
    countAllocations = false;
    require(allocations == 0, "process allocated memory");
    require(diagnostics.candidateValid, "aligned candidate was unavailable");
    require(engine.aligned() && engine.delaySamples() == delay,
            "sample rate matrix alignment failed");
    require(energy(output, rate * 3 / 2, count) <
                energy(mic, rate * 3 / 2, count) * 0.01,
            "sample rate matrix cancellation failed");
    countAllocations = true;
    engine.process(mic.data(), nullptr, output.data(), count);
    engine.reset();
    countAllocations = false;
    require(allocations == 0, "discontinuity handling allocated memory");
  }
}

std::vector<float> harmonics(int count, int rate, double pitch,
                             std::uint32_t seed) {
  const auto phases = noise(5, seed);
  std::vector<float> result(count);
  constexpr double pi = 3.141592653589793;
  for (int i = 0; i < count; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double envelope =
        0.2 + 0.8 * std::pow(std::max(0.0, std::sin(2 * pi * 3.5 * t)), 2);
    const double f0 = pitch * (1 + 0.06 * std::sin(2 * pi * 1.3 * t));
    double value = 0;
    for (int k = 0; k < 5; ++k) {
      value += std::sin(2 * pi * f0 * (k + 1) * t + phases[k] * 12) / (k + 1);
    }
    result[i] = static_cast<float>(0.36 * envelope * value / 2.25);
  }
  return result;
}

void modulatedHarmonicDoubleTalk() {
  // Independent development reproducer: colored periodic input, envelope
  // gaps and an echo tail longer than the baseline filter. This exposed
  // divergence that stationary broadband tests did not exercise.
  constexpr int rate = 48000;
  constexpr int count = rate * 3;
  auto reference = harmonics(count, rate, 165, 97);
  auto near = harmonics(count, rate, 121, 101);
  std::fill(reference.begin() + count * 7 / 10, reference.end(), 0);
  std::fill(near.begin(), near.begin() + count * 4 / 10, 0);
  for (const bool stronglyCoupled : {false, true}) {
    const int delay = stronglyCoupled ? 288 : 1104;
    std::vector<float> mic(count);
    std::vector<float> output(count);
    const auto source = [&](int i) { return i >= 0 ? reference[i] : 0.0F; };
    for (int i = 0; i < count; ++i) {
      const float echo = stronglyCoupled
                             ? 1.5F * (0.93F * source(i - delay) +
                                       0.28F * source(i - delay - 192) -
                                       0.19F * source(i - delay - 864))
                             : 0.68F * (0.75F * source(i - delay) +
                                        0.34F * source(i - delay - 576) -
                                        0.19F * source(i - delay - 2256));
      mic[i] = echo + near[i];
    }
    buncancel::Aec engine(rate);
    engine.process(mic.data(), reference.data(), output.data(), count);
    double dryError = 0;
    double wetError = 0;
    double cross = 0;
    double nearEnergy = 0;
    for (int i = count * 4 / 10; i < count * 7 / 10; ++i) {
      dryError += std::pow(mic[i] - near[i], 2);
      wetError += std::pow(output[i] - near[i], 2);
      cross += static_cast<double>(output[i]) * near[i];
      nearEnergy += static_cast<double>(near[i]) * near[i];
      require(std::abs(output[i]) < 1, "harmonic double talk caused overload");
    }
    require(cross / nearEnergy > 0.9,
            "harmonic double talk attenuated near-end voice");
    require(wetError <= dryError * 1.1,
            "harmonic double talk amplified interference");
  }
}

void acquisitionDiagnostics() {
  using State = buncancel::Aec::AlignmentState;
  constexpr int rate = 8000;
  constexpr int count = rate * 2;
  const auto reference = noise(count, 107);
  std::vector<float> mic(count);
  std::vector<float> output(count);
  buncancel::Aec engine(rate);
  require(engine.diagnostics().state == State::Warmup,
          "new engine did not report warmup");
  engine.process(reference.data(), reference.data(), output.data(), rate / 2);
  require(engine.diagnostics().state == State::Warmup,
          "history accumulation was misreported as failed correlation");
  engine.process(reference.data() + rate / 2, reference.data() + rate / 2,
                 output.data() + rate / 2, rate / 5);
  require(engine.diagnostics().state == State::Searching,
          "first unfinished scan was misreported as failed correlation");

  for (const int delay : {-80, rate * 9 / 20}) {
    for (int i = 0; i < count; ++i) {
      const int source = i - delay;
      mic[i] = source >= 0 && source < count ? reference[source] * 0.7F : 0;
    }
    engine.reset();
    engine.process(mic.data(), reference.data(), output.data(), count);
    const auto diagnostics = engine.diagnostics();
    require(!engine.aligned() && output == mic,
            "excluded delay modified the dry microphone");
    require(diagnostics.state == State::DelayOutOfRange &&
                diagnostics.candidateValid && diagnostics.peakValid &&
                diagnostics.candidateLagSamples == delay &&
                diagnostics.peakLagSamples == delay &&
                diagnostics.peakCorrelation > 0.99F,
            "confirmed excluded delay was not reported accurately");
  }

  mic = noise(count, 109);
  engine.reset();
  engine.process(mic.data(), reference.data(), output.data(), count);
  auto diagnostics = engine.diagnostics();
  require(diagnostics.state == State::InsufficientCorrelation &&
              !diagnostics.candidateValid && diagnostics.peakValid &&
              diagnostics.peakLagSamples != 0 && !diagnostics.reductionValid,
          "weak correlation was mislabeled as excluded delay or reduction");
  require(output == mic, "unrelated reference modified the microphone");

  engine.reset();
  engine.process(reference.data(), reference.data(), output.data(), count);
  diagnostics = engine.diagnostics();
  require(diagnostics.state == State::SelfMonitor &&
              diagnostics.candidateValid &&
              diagnostics.candidateLagSamples == 0,
          "direct self-monitoring was not diagnosed");
  std::vector<float> tone(count);
  for (int i = 0; i < count; ++i) {
    tone[i] =
        static_cast<float>(0.2 * std::sin(i * 2 * 3.141592653589793 / 32));
    mic[i] = i >= 200 ? tone[i - 200] * 0.7F : 0;
  }
  engine.reset();
  engine.process(mic.data(), tone.data(), output.data(), count);
  require(engine.diagnostics().state == State::Ambiguous && output == mic,
          "periodic ambiguity was not diagnosed safely");
  engine.process(mic.data(), nullptr, output.data(), count);
  diagnostics = engine.diagnostics();
  require(diagnostics.state == State::ReferenceMissing &&
              !diagnostics.candidateValid && !diagnostics.peakValid &&
              !diagnostics.reductionValid,
          "reference loss retained stale diagnostic evidence");
}

void echoDisappearance() {
  constexpr int rate = 8000;
  constexpr int count = rate * 4;
  constexpr int delay = 211;
  const auto reference = noise(count, 19);
  const auto near = noise(count, 29);
  std::vector<float> mic(count);
  std::vector<float> output(count);
  for (int i = 0; i < count; ++i) {
    mic[i] =
        i < rate * 2 ? (i >= delay ? reference[i - delay] * 0.6F : 0) : near[i];
  }
  buncancel::Aec engine(rate);
  engine.process(mic.data(), reference.data(), output.data(), rate * 2);
  auto diagnostics = engine.diagnostics();
  require(diagnostics.reductionValid && diagnostics.reductionDb > 20,
          "converged coherent subtraction lacked reduction evidence");
  engine.process(mic.data() + rate * 2, reference.data() + rate * 2,
                 output.data() + rate * 2, rate / 5);
  require(!engine.diagnostics().reductionValid,
          "echo disappearance retained stale reduction evidence");
  engine.process(mic.data() + rate * 11 / 5, reference.data() + rate * 11 / 5,
                 output.data() + rate * 11 / 5, count - rate * 11 / 5);
  double error = 0;
  double cross = 0;
  for (int i = rate * 2; i < count; ++i) {
    error += std::pow(output[i] - near[i], 2);
    cross += static_cast<double>(output[i]) * near[i];
  }
  const double nearEnergy = energy(near, rate * 2, count);
  require(error < nearEnergy * 0.025 && cross / nearEnergy > 0.98,
          "disappearing echo injected interference or damaged near voice");
  require(std::equal(output.begin() + rate * 3, output.end(),
                     mic.begin() + rate * 3),
          "echo disappearance did not recover dry microphone");
  require(!engine.diagnostics().reductionValid,
          "dry microphone claimed ongoing reduction");
}

void trainedMicrophoneMute() {
  constexpr int rate = 8000;
  constexpr int count = rate * 3;
  constexpr int delay = 211;
  const auto reference = noise(count, 127);
  const auto near = noise(count, 131);
  std::vector<float> mic(count);
  std::vector<float> output(count);
  for (int i = delay; i < rate * 2; ++i) {
    mic[i] = reference[i - delay] * 0.7F;
  }
  buncancel::Aec engine(rate);
  engine.process(mic.data(), reference.data(), output.data(), rate * 2);
  require(engine.aligned() && engine.diagnostics().reductionValid,
          "microphone mute test did not train its echo model");
  for (const int block : {1, 31, 128, 2048}) {
    const int start = rate * 2;
    engine.process(mic.data() + start, reference.data() + start,
                   output.data() + start, block);
    require(std::all_of(output.begin() + start, output.begin() + start + block,
                        [](float sample) { return sample == 0; }),
            "trained echo model emitted audio into digitally muted mic");
    require(!engine.diagnostics().reductionValid,
            "digitally muted microphone claimed current reduction");
  }

  // A quiet, nonzero microphone is a distinct signal, not a digital mute.
  engine.reset();
  for (int i = 0; i < count; ++i) {
    mic[i] = near[i] * 1.0e-5F;
  }
  engine.process(mic.data(), reference.data(), output.data(), count);
  require(output == mic, "digital mute protection gated quiet near speech");
}
void postAlignmentLearning() {
  // Field-level reference and a 252 ms effective lag. A reflection changes
  // after the foreground has warmed up, leaving that old model below the
  // 0.98 adaptation-coherence gate despite a correct stable delay.
  constexpr int rate = 48000;
  constexpr int count = rate * 8;
  constexpr int delay = 12096;
  auto reference = noise(count, 72);
  for (auto &sample : reference) {
    sample *= 0.086F;
  }
  std::vector<float> mic(count), output(count);
  for (int i = delay + 32; i < count; ++i) {
    mic[i] = 1.2F * reference[i - delay] +
             (i < rate * 3 ? 0.05F : 0.9F) * reference[i - delay - 32];
  }
  buncancel::Aec engine(rate);
  engine.process(mic.data(), reference.data(), output.data(), count - 256);
  const auto diagnostic = engine.diagnostics();
  require(engine.aligned() && engine.delaySamples() == delay,
          "changed reflection lost a supported delay");
  require(energy(output, rate * 6, count) < energy(mic, rate * 6, count) * 0.01,
          "post-warmup coherence freeze prevented path relearning");
  require(diagnostic.shadowPromotions > 0 &&
              diagnostic.shadowAdaptedSamples > 0 &&
              diagnostic.adaptedSamples >= 2048 && diagnostic.weightsNorm > 1,
          "validated shadow learning lacked diagnostic evidence");
  require(diagnostic.liveConfidence > 0.65F && diagnostic.predictionRms > 0 &&
              diagnostic.microphoneRms > 0 &&
              diagnostic.outputRms < diagnostic.microphoneRms * 0.1F &&
              diagnostic.subtractionGain > 0 && diagnostic.subtractionActive &&
              diagnostic.samplesSinceReset > static_cast<unsigned>(rate) &&
              diagnostic.filterResetCount > 0,
          "active filter diagnostics did not describe processed audio");
  // A valid audio block may end in an exact zero crossing. Activity belongs
  // to the processed block, rather than the numerical value of its last sample.
  mic[count - 1] = 0;
  reference[count - 1] = 0;
  engine.process(mic.data() + count - 256, reference.data() + count - 256,
                 output.data() + count - 256, 256);
  require(engine.diagnostics().subtractionActive &&
              engine.diagnostics().reductionValid,
          "a trailing digital zero hid real current-block subtraction");
  const auto resets = diagnostic.filterResetCount;
  engine.process(mic.data(), nullptr, output.data(), rate / 10);
  const auto dry = engine.diagnostics();
  require(std::equal(output.begin(), output.begin() + rate / 10, mic.begin()) &&
              dry.filterResetCount > resets &&
              dry.samplesSinceReset == static_cast<unsigned>(rate / 10) &&
              dry.adaptedSamples == 0 && dry.weightsNorm == 0 &&
              dry.predictionRms == 0 && !dry.subtractionActive &&
              dry.shadowValidationCount == 0 &&
              dry.shadowDecision ==
                  buncancel::Aec::ShadowDecision::waitingAlignment,
          "reference loss retained a learned or active filter");
}

void quietReferenceEvidence() {
  constexpr int rate = 48000;
  constexpr int count = rate * 5;
  constexpr int delay = 12096; // 252 ms DSP lag plus 20 ms reference holdback.
  auto reference = noise(count, 1739);
  const auto independent = noise(count, 9157);
  const float targetRms = std::pow(10.0F, -74.7F / 20);
  const float scale =
      targetRms /
      static_cast<float>(std::sqrt(energy(reference, 0, count) / count));
  for (auto &sample : reference)
    sample *= scale;
  std::vector<float> mic(count), output(count);
  for (int i = delay; i < count; ++i)
    mic[i] = reference[i - delay] * 0.7F;
  buncancel::Aec correlated(rate);
  correlated.process(mic.data(), reference.data(), output.data(), count);
  const auto valid = correlated.diagnostics();
  require(correlated.aligned() && correlated.delaySamples() == delay &&
              energy(output, rate * 4, count) <
                  energy(mic, rate * 4, count) * 0.01,
          "quiet but correlated reference failed validated subtraction");
  require(valid.shadowValidationCount > 0 && valid.shadowPromotions > 0 &&
              valid.shadowTrainingEligibility > 0.9F &&
              valid.shadowValidationEligibility > 0.9F &&
              valid.shadowCoherence > 0.9F &&
              std::isfinite(valid.shadowMicErrorRatio) &&
              std::isfinite(valid.shadowForegroundErrorRatio),
          "quiet candidate validation lacked measurable evidence");

  // Match the screenshot levels while the strongest real relationship is
  // only about 0.5. That weak relationship must not authorize subtraction.
  const float noiseScale =
      targetRms /
      static_cast<float>(std::sqrt(energy(independent, 0, count) / count));
  for (int i = 0; i < count; ++i) {
    const float echo = i >= delay ? reference[i - delay] : 0;
    mic[i] =
        0.7F * (0.5F * echo + std::sqrt(0.75F) * noiseScale * independent[i]);
  }
  buncancel::Aec weak(rate);
  weak.process(mic.data(), reference.data(), output.data(), count);
  const auto rejected = weak.diagnostics();
  require(!weak.aligned() && output == mic &&
              rejected.shadowDecision ==
                  buncancel::Aec::ShadowDecision::waitingAlignment &&
              rejected.shadowPromotions == 0,
          "quiet weak correlation modified the independent microphone");
  // A previously valid model can remain aligned during double-talk while the
  // candidate learner correctly lacks enough far-end evidence to replace it.
  for (int i = 0; i < count; ++i) {
    const float echo = reference[(i + count - delay) % count];
    mic[i] = 0.7F * echo + 1.4F * noiseScale * independent[i];
  }
  correlated.process(mic.data(), reference.data(), output.data(), count);
  const auto held = correlated.diagnostics();
  require(correlated.aligned() && held.liveConfidence > 0.2F &&
              held.liveConfidence < 0.65F &&
              held.shadowDecision ==
                  buncancel::Aec::ShadowDecision::insufficientEvidence &&
              held.shadowValidationEligibility < 0.9F &&
              held.doubleTalkFreezeRatio > 0.95F,
          "aligned double-talk lacked a specific candidate rejection reason");
  double projection = 0;
  double nearEnergy = 0;
  for (int i = rate * 4; i < count; ++i) {
    const double near = 1.4F * noiseScale * independent[i];
    projection += near * output[i];
    nearEnergy += near * near;
  }
  require(projection / nearEnergy > 0.98 && projection / nearEnergy < 1.02,
          "quiet aligned double-talk damaged the independent near component");
}

void repeatedBroadbandAmbiguity() {
  constexpr int rate = 48000;
  constexpr int count = rate * 3;
  constexpr int period = 576;
  constexpr int delay = 12209;
  const auto motif = noise(period, 811);
  std::vector<float> reference(count), mic(count), output(count);
  for (int i = 0; i < count; ++i) {
    reference[i] = motif[i % period];
    mic[i] = i >= delay ? reference[i - delay] * 0.7F : 0;
  }
  buncancel::Aec engine(rate);
  engine.process(mic.data(), reference.data(), output.data(), count);
  const auto diagnostic = engine.diagnostics();
  require(!engine.aligned() && output == mic,
          "separated periodic broadband aliases modified the microphone");
  require(diagnostic.state == buncancel::Aec::AlignmentState::Ambiguous &&
              diagnostic.competingPeakCorrelation > 0.9F &&
              !diagnostic.candidateValid && !diagnostic.subtractionActive,
          "separated delay aliases were not diagnosed as ambiguous");
}

} // namespace

int main() {
  try {
    cancellationAndBlocks();
    doubleTalk();
    longDelayAndReferenceLoss();
    safety();
    unusableAlignment();
    sampleRatesAndAllocation();
    modulatedHarmonicDoubleTalk();
    acquisitionDiagnostics();
    echoDisappearance();
    trainedMicrophoneMute();
    postAlignmentLearning();
    repeatedBroadbandAmbiguity();
    quietReferenceEvidence();
    std::cout << "AEC deterministic regression checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << "AEC regression failed: " << error.what() << '\n';
    return 1;
  }
}
