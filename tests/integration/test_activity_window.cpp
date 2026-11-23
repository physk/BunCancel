#include "activity_window.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using buncancel::plugin::ActivityWindow;
using buncancel::plugin::SignalMoments;
constexpr std::uint64_t epoch = 10000000000ULL;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void near(double actual, double expected, double tolerance,
          const char *message) {
  require(std::abs(actual - expected) <= tolerance, message);
}

SignalMoments constant(std::uint64_t samples, float amplitude) {
  return {static_cast<double>(amplitude) * amplitude * samples,
          std::abs(amplitude), samples};
}

float dbfs(double decibels) {
  return static_cast<float>(std::pow(10.0, decibels / 20));
}

std::uint64_t time(double seconds) {
  return epoch + static_cast<std::uint64_t>(std::llround(seconds * 10000000));
}

void quietScreenshot() {
  ActivityWindow window;
  const auto mic = constant(512, dbfs(-77.9));
  const auto raw = constant(1024, dbfs(-74.7));
  const auto downmix = constant(512, dbfs(-74.7));
  window.observeMicrophone(time(0), 48000, 512, mic, mic, 0, true, false);
  window.observeReference(time(0), 48000, 512, raw, downmix);
  const auto snapshot = window.snapshot(time(0));
  require(snapshot.mic_observed && snapshot.reference_observed,
          "quiet actual PCM was confused with absent data");
  require(snapshot.reference_receipt_valid &&
              snapshot.reference_receipt_age_ms == 0,
          "fresh quiet reference packet has wrong receipt age");
  require(!snapshot.mic_activity_valid && !snapshot.reference_activity_valid,
          "screenshot noise floor was labelled above-threshold activity");
  near(20 * std::log10(snapshot.mic_rms), -77.9, 0.001,
       "quiet microphone RMS changed");
  near(20 * std::log10(snapshot.reference_rms), -74.7, 0.001,
       "quiet reference RMS changed");
  require(snapshot.changed_fraction == 0 && snapshot.usable_fraction == 1 &&
              snapshot.subtraction_fraction == 0 &&
              snapshot.mic_active_fraction == 0 &&
              snapshot.reference_active_fraction == 0,
          "quiet dry evidence reported subtraction");
}

void intermittentPlayback() {
  ActivityWindow window;
  constexpr std::uint32_t frames = 480;
  const auto strong = constant(frames, dbfs(-34));
  const auto weak = constant(frames, dbfs(-78));
  for (int block = 0; block < 300; ++block) {
    const double second = block * 0.01;
    const bool audible = block >= 100 && block < 200;
    const auto mic = audible ? strong : weak;
    const auto output = audible ? constant(frames, dbfs(-44)) : mic;
    window.observeMicrophone(time(second), 48000, frames, mic, output,
                             audible ? frames : 0, true, audible);
    window.observeReference(time(second), 48000, frames,
                            audible ? constant(2 * frames, dbfs(-38))
                                    : constant(2 * frames, dbfs(-75)),
                            audible ? constant(frames, dbfs(-38))
                                    : constant(frames, dbfs(-75)));
  }
  const auto snapshot = window.snapshot(time(2.99));
  near(snapshot.mic_seconds, 3, 0.000001,
       "evidence window lost actual microphone duration");
  near(snapshot.reference_seconds, 3, 0.000001,
       "evidence window lost actual reference duration");
  const double expected =
      std::sqrt((strong.energy * 100 + weak.energy * 200) / (frames * 300));
  near(snapshot.mic_rms, expected, 0.0000001,
       "rolling energy was replaced by the quiet final callback");
  require(snapshot.mic_rms > weak.peak * 80,
          "audible recent speech disappeared from rolling evidence");
  near(snapshot.changed_fraction, 1.0 / 3, 0.000001,
       "changed output fraction lost earlier active processing");
  near(snapshot.subtraction_fraction, 1.0 / 3, 0.000001,
       "subtraction fraction was extrapolated from the last block");
  near(snapshot.mic_active_fraction, 1.0 / 3, 0.000001,
       "microphone activity fraction ignored quiet observations");
  near(snapshot.reference_active_fraction, 1.0 / 3, 0.000001,
       "reference activity fraction ignored quiet packet PCM");
  near(snapshot.mic_activity_age_ms, 1000, 0.0001,
       "mic activity age used quiet packet receipt time");
  near(snapshot.reference_activity_age_ms, 1000, 0.0001,
       "reference activity age used quiet packet receipt time");
  near(snapshot.reference_receipt_age_ms, 0, 0.0001,
       "reference packet arrival age was confused with activity age");
  const auto expired = window.snapshot(time(6));
  require(!expired.mic_observed && !expired.reference_observed &&
              expired.mic_rms == 0 && expired.reference_rms == 0,
          "old PCM evidence persisted beyond the rolling window");
  near(expired.reference_receipt_age_ms, 3010, 0.0001,
       "packet receipt age failed to advance after capture stopped");
  near(expired.reference_activity_age_ms, 4010, 0.0001,
       "activity age failed to advance independently after capture stopped");
}

void sampleWeightAndDownmix() {
  ActivityWindow window;
  window.observeMicrophone(time(0), 48000, 64, constant(64, 0.2F),
                           constant(64, 0.1F), 64, true, true);
  window.observeMicrophone(time(0.01), 48000, 512, constant(512, 0.1F),
                           constant(512, 0.1F), 0, false, false);
  // Anti-phase stereo keeps the raw PCM active while its actual downmix is
  // zero. The UI can distinguish this route from missing or silent source PCM.
  window.observeReference(time(0.01), 48000, 480, constant(960, 0.2F),
                          constant(480, 0));
  const auto snapshot = window.snapshot(time(0.02));
  near(snapshot.changed_fraction, 64.0 / 576, 0.000001,
       "evidence fraction weighted callbacks instead of samples");
  near(snapshot.usable_fraction, 64.0 / 576, 0.000001,
       "reference usability fraction lost invalid callback");
  near(snapshot.mic_rms, std::sqrt((64 * 0.04 + 512 * 0.01) / 576), 0.000001,
       "RMS weighted callbacks instead of sample energy");
  near(snapshot.reference_rms, 0.2, 0.000001, "raw stereo RMS incorrect");
  require(snapshot.downmix_rms == 0 && snapshot.downmix_peak == 0 &&
              snapshot.reference_activity_valid,
          "anti-phase downmix was confused with source silence");
  near(snapshot.reference_seconds, 0.01, 0.000001,
       "stereo reference duration was doubled by channel count");
  window.reset();
  const auto reset = window.snapshot(time(0.03));
  require(!reset.mic_observed && !reset.reference_observed &&
              !reset.reference_receipt_valid && !reset.mic_activity_valid &&
              !reset.reference_activity_valid,
          "source reset retained another endpoint's evidence");
}

void noHeldReferenceAndInvalidInput() {
  ActivityWindow window;
  const auto block = constant(480, 0.1F);
  window.observeReference(time(0), 48000, 480, block, block);
  for (int i = 0; i < 200; ++i) {
    window.observeMicrophone(time(i * 0.01), 48000, 480, block, block, 0, false,
                             false);
  }
  const auto snapshot = window.snapshot(time(2));
  near(snapshot.reference_seconds, 0.01, 0.000001,
       "held reference level was counted as newly captured samples");
  near(snapshot.reference_receipt_age_ms, 2000, 0.0001,
       "mic callbacks refreshed reference packet age");
  window.observeReference(time(1), 48000, 480, {NAN, 0.1F, 480}, block);
  window.observeReference(time(1), 48000, 480, block, {0.1, INFINITY, 480});
  window.observeReference(time(1), 0, 480, block, block);
  window.observeMicrophone(time(0.5), 48000, 480, block, block, 480, true,
                           true);
  const auto invalid = window.snapshot(time(2));
  near(invalid.reference_receipt_age_ms, 2000, 0.0001,
       "invalid reference input refreshed arrival evidence");
  near(invalid.mic_seconds, snapshot.mic_seconds, 0.000001,
       "backward timestamp was accepted into rolling evidence");
  require(invalid.changed_fraction == 0,
          "backward observation introduced false cancellation evidence");
  require(!window.snapshot(0).mic_observed,
          "invalid snapshot clock produced observed data");
}
} // namespace

int main() {
  static_assert(std::is_trivially_destructible_v<ActivityWindow>);
  static_assert(sizeof(ActivityWindow) < 16384);
  try {
    quietScreenshot();
    intermittentPlayback();
    sampleWeightAndDownmix();
    noHeldReferenceAndInvalidInput();
    std::cout << "rolling activity evidence tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "rolling activity: " << error.what() << '\n';
    return 1;
  }
}
