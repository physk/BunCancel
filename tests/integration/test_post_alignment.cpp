#include "aec.hpp"
#include "diagnostics.hpp"
#include "host_clock.hpp"
#include "reference_timeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int rate = 48000;
constexpr std::uint64_t epoch = 216000000000ULL;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::uint64_t stamp(double frame) {
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(epoch) +
                                    std::llround(frame * 1.0e7 / rate));
}

// Redistributable synthetic development stimuli, with independent deterministic
// near/render seeds. No recorded voice or held-out evaluation material is used.
class Stimulus {
public:
  Stimulus(int frames, std::uint32_t seed, bool speechLike = false)
      : speechLike_(speechLike), frequency_(120 + seed % 89),
        values_((frames + rate) / 6 + 32) {
    for (float &value : values_) {
      seed = seed * 1664525U + 1013904223U;
      value = (static_cast<float>(seed >> 8) / 16777216.0F - 0.5F) * 0.7F;
    }
  }

  float sample(double frame) const {
    frame += rate;
    if (frame < 0) {
      return 0;
    }
    const auto index = static_cast<std::size_t>(frame / 6);
    if (index + 1 >= values_.size()) {
      return 0;
    }
    const float fraction = static_cast<float>(frame / 6 - index);
    const float broadband =
        values_[index] * (1 - fraction) + values_[index + 1] * fraction;
    if (!speechLike_) {
      return broadband;
    }
    const double time = frame / rate;
    constexpr double pi = 3.14159265358979323846;
    const double phase =
        2 * pi * frequency_ * time - 20 / 0.37 * std::cos(2 * pi * 0.37 * time);
    const double envelope =
        0.25 + 0.75 * std::pow(std::sin(2 * pi * 2.3 * time), 2);
    return static_cast<float>(envelope *
                              (0.7 * broadband + 0.07 * std::sin(phase) +
                               0.04 * std::sin(2 * phase)));
  }

private:
  bool speechLike_{};
  double frequency_{};
  std::vector<float> values_;
};

struct FieldWindow {
  double micEnergy{};
  double outputEnergy{};
  double nearEnergy{};
  double nearCross{};
  double errorEnergy{};
  std::uint64_t samples{};
  std::uint64_t changed{};

  void add(float mic, float output, float near) {
    micEnergy += static_cast<double>(mic) * mic;
    outputEnergy += static_cast<double>(output) * output;
    nearEnergy += static_cast<double>(near) * near;
    nearCross += static_cast<double>(near) * output;
    const double error = output - near;
    errorEnergy += error * error;
    ++samples;
    changed += output != mic;
  }

  double erle() const {
    return 10 * std::log10((micEnergy + 1.0e-30) / (outputEnergy + 1.0e-30));
  }
};

void writeFieldWav(const std::filesystem::path &path,
                   const std::vector<float> &samples, int channels) {
  std::ofstream file(path, std::ios::binary);
  require(static_cast<bool>(file), "cannot open field evidence WAV");
  const auto little = [&file](std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
      file.put(static_cast<char>((value >> (8 * i)) & 255));
    }
  };
  const auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
  file.write("RIFF", 4);
  little(bytes + 36, 4);
  file.write("WAVEfmt ", 8);
  little(16, 4);
  little(1, 2);
  little(channels, 2);
  little(rate, 4);
  little(rate * channels * 2, 4);
  little(channels * 2, 2);
  little(16, 2);
  file.write("data", 4);
  little(bytes, 4);
  for (float sample : samples) {
    const auto pcm = static_cast<std::int16_t>(
        std::lround(std::clamp(sample, -1.0F, 1.0F) * 32767));
    little(static_cast<std::uint16_t>(pcm), 2);
  }
  require(static_cast<bool>(file), "cannot write field evidence WAV");
}

void fieldLevelPostAlignment(bool changePath, bool withLoss,
                             bool invertPath = false) {
  constexpr int frames = 16 * rate;
  constexpr int physicalDelay = 13056; // 272 ms, before 20 ms holdback.
  const Stimulus first(frames + rate, 197, true);
  const Stimulus second(frames + rate, 373, true);
  const Stimulus near(frames + rate, 991, true);
  std::vector<float> stereo(frames * 2), mono(frames), microphone(frames),
      desired(frames), processed(frames);
  double referenceEnergy = 0;
  double voiceEnergy = 0;
  for (int i = 0; i < frames; ++i) {
    // Related stereo program with a distinct right channel. The reference
    // timeline uses the same explicit 50/50 downmix as the physical path.
    stereo[2 * i] = first.sample(i);
    stereo[2 * i + 1] = 0.75F * first.sample(i) + 0.25F * second.sample(i);
    mono[i] = 0.5F * (stereo[2 * i] + stereo[2 * i + 1]);
    referenceEnergy += static_cast<double>(mono[i]) * mono[i];
    voiceEnergy += static_cast<double>(near.sample(i)) * near.sample(i);
  }
  const auto referenceScale = static_cast<float>(
      std::pow(10.0, -38.1 / 20) / std::sqrt(referenceEnergy / frames));
  const auto nearScale = static_cast<float>(std::pow(10.0, -32.0 / 20) /
                                            std::sqrt(voiceEnergy / frames));
  for (float &sample : stereo) {
    sample *= referenceScale;
  }
  for (float &sample : mono) {
    sample *= referenceScale;
  }
  const auto render = [&mono](int time) {
    return time < 0 ? 0.0F : mono[static_cast<std::size_t>(time)];
  };
  for (int i = 0; i < frames; ++i) {
    // The dominant direct path permits independent correlation evidence.
    // A changed 17-sample reflection exercises recovery without moving the
    // bulk delay. Reflections at 9 and 19 ms exceed the 2.67 ms model tail.
    const float direct = !changePath || i < 6 * rate ? 1.70F : 1.25F;
    const float reflection = !changePath || i < 6 * rate ? -0.22F : 0.85F;
    const float echo = direct * render(i - physicalDelay) +
                       reflection * render(i - physicalDelay - 17) +
                       0.34F * render(i - physicalDelay - 432) +
                       0.16F * render(i - physicalDelay - 912);
    const float gain = i < 6 * rate ? 1.0F : invertPath ? -0.55F : 0.55F;
    const bool nearActive = i >= 10 * rate;
    desired[i] = nearActive ? nearScale * near.sample(i) : 0.0F;
    // The final two seconds contain independent near speech without leakage.
    microphone[i] = desired[i] + (i < 14 * rate ? gain * echo : 0.0F);
  }

  buncancel::capture::ReferenceTimelineConfig config;
  config.sample_rate = rate;
  config.max_block_frames = 256;
  config.holdback_frames = 960;
  buncancel::capture::ReferenceTimeline timeline(config);
  buncancel::plugin::HostClock hostClock(rate);
  buncancel::Aec engine(rate);
  std::array<float, 256> alignedReference{};
  constexpr std::array<int, 5> packetSizes{480, 240, 360, 480, 120};
  int captureFrame = 0;
  int packetSequence = 0;
  bool discontinuity = false;
  int missing = 0;
  int resets = 0;
  FieldWindow initial, changed, overlap, nearOnly;
  bool alignedAfterChange = false;
  float liveAfterChange = 0;
  int lagAfterChange = 0;
  for (int start = 0; start < frames; start += 256) {
    const int count = std::min(256, frames - start);
    const int callbackEnd = start + count;
    const auto clock =
        hostClock.observe(stamp(callbackEnd), count, true, start);
    while (captureFrame < frames) {
      const int packetFrames =
          std::min(packetSizes[packetSequence % packetSizes.size()],
                   frames - captureFrame);
      const int delivery =
          captureFrame + packetFrames + 48 + (packetSequence % 4) * 24;
      if (delivery > callbackEnd) {
        break;
      }
      const bool gap =
          withLoss && captureFrame >= 8 * rate && captureFrame < 8 * rate + 240;
      if (!gap) {
        buncancel::capture::ReferencePacket packet;
        packet.frame_count = packetFrames;
        packet.channel_count = 2;
        packet.sample_rate = rate;
        packet.device_position = captureFrame + 9000000;
        packet.qpc_position_100ns = stamp(captureFrame);
        packet.generation = 3;
        packet.discontinuity = discontinuity;
        for (int i = 0; i < packetFrames * 2; ++i) {
          packet.samples[i] = stereo[2 * captureFrame + i];
        }
        timeline.push(packet);
        discontinuity = false;
      } else {
        discontinuity = true;
      }
      captureFrame += packetFrames;
      ++packetSequence;
    }
    const auto mapping = timeline.render(clock.microphone_start_qpc_100ns,
                                         count, alignedReference.data());
    if (mapping.reset_required) {
      engine.reset();
      ++resets;
    }
    missing += !mapping.usable;
    engine.process(microphone.data() + start,
                   mapping.usable ? alignedReference.data() : nullptr,
                   processed.data() + start, count);
    if (start < 10 * rate && start + count >= 10 * rate) {
      alignedAfterChange = engine.aligned();
      liveAfterChange = engine.alignmentConfidence();
      lagAfterChange = engine.delaySamples();
    }
    for (int i = 0; i < count; ++i) {
      const int time = start + i;
      require(std::isfinite(processed[time]), "field path emitted invalid PCM");
      if (!mapping.usable) {
        require(processed[time] == microphone[time],
                "field path failed exact dry output during capture gap");
      }
      FieldWindow *window = nullptr;
      if (time >= 4 * rate && time < 6 * rate) {
        window = &initial;
      } else if (time >= 9 * rate && time < 10 * rate) {
        window = &changed;
      } else if (time >= 12 * rate && time < 14 * rate) {
        window = &overlap;
      } else if (time >= 15 * rate) {
        window = &nearOnly;
      }
      if (window) {
        window->add(microphone[time], processed[time], desired[time]);
      }
    }
  }
  const auto diagnostic = engine.diagnostics();
  const std::string name = !changePath  ? "control"
                           : invertPath ? "polarity_and_path_change"
                           : withLoss   ? "path_change_gap"
                                        : "path_change";
  std::cout << "field_post_alignment_" << name
            << " physical_delay_ms=272 reference_rms_dbfs=-38.1"
            << " mic_far_rms_dbfs="
            << 10 * std::log10(initial.micEnergy / initial.samples)
            << " initial_far_erle_db=" << initial.erle()
            << " after_change_far_erle_db=" << changed.erle()
            << " after_change_aligned=" << alignedAfterChange
            << " after_change_live_confidence=" << liveAfterChange
            << " after_change_lag_samples=" << lagAfterChange
            << " far_changed_fraction="
            << static_cast<double>(changed.changed) / changed.samples
            << " double_talk_near_gain="
            << overlap.nearCross / overlap.nearEnergy
            << " double_talk_error_to_near="
            << overlap.errorEnergy / overlap.nearEnergy
            << " near_only_gain=" << nearOnly.nearCross / nearOnly.nearEnergy
            << " near_only_error_to_near="
            << nearOnly.errorEnergy / nearOnly.nearEnergy
            << " final_live_confidence=" << engine.alignmentConfidence()
            << " final_peak_correlation=" << diagnostic.peakCorrelation
            << " missing_blocks=" << missing << " resets=" << resets << '\n';
  if (const char *directory = std::getenv("BUNCANCEL_FIELD_EXPORT")) {
    const auto path = std::filesystem::path(directory) / name;
    std::filesystem::create_directories(path);
    writeFieldWav(path / "reference.wav", stereo, 2);
    writeFieldWav(path / "mic_input.wav", microphone, 1);
    writeFieldWav(path / "clean_voice.wav", desired, 1);
    writeFieldWav(path / "processed.wav", processed, 1);
    std::vector<float> echo(frames);
    for (int i = 0; i < frames; ++i) {
      echo[i] = microphone[i] - desired[i];
    }
    writeFieldWav(path / "echo_only.wav", echo, 1);
    std::ofstream report(path / "metrics.json");
    report << std::setprecision(17) << "{\n  \"scenario\": \"" << name
           << "\",\n"
           << "  \"sample_rate\": 48000,\n"
           << "  \"physical_delay_samples\": 13056,\n"
           << "  \"reference_holdback_samples\": 960,\n"
           << "  \"reference_rms_dbfs\": -38.1,\n"
           << "  \"initial_far_erle_db\": " << initial.erle() << ",\n"
           << "  \"after_change_far_erle_db\": " << changed.erle() << ",\n"
           << "  \"after_change_live_confidence\": " << liveAfterChange << ",\n"
           << "  \"after_change_lag_samples\": " << lagAfterChange << ",\n"
           << "  \"double_talk_near_projection\": "
           << overlap.nearCross / overlap.nearEnergy << ",\n"
           << "  \"double_talk_error_to_near_energy\": "
           << overlap.errorEnergy / overlap.nearEnergy << ",\n"
           << "  \"near_only_projection\": "
           << nearOnly.nearCross / nearOnly.nearEnergy << ",\n"
           << "  \"near_only_error_to_near_energy\": "
           << nearOnly.errorEnergy / nearOnly.nearEnergy << ",\n"
           << "  \"missing_blocks\": " << missing << ",\n"
           << "  \"timeline_reset_blocks\": " << resets << "\n}\n";
    std::ofstream manifest(path / "manifest.json");
    manifest << R"JSON({
  "dataset_split": "development",
  "source": "Project-generated modulated harmonics and deterministic interpolated noise",
  "license": "MIT, project-authored synthetic audio",
  "render_seeds": [
    197,
    373
  ],
  "near_seed": 991,
  "sample_rate": 48000,
  "duration_seconds": 16,
  "reference_channels": 2,
  "microphone_channels": 1,
  "reference_downmix": "0.5 * left + 0.5 * right",
  "encoding": "PCM16, round(clamp(sample, -1, 1) * 32767)",
  "timebase": "microphone input samples, zero output latency",
  "physical_delay_samples": 13056,
  "filter_holdback_samples": 960,
  "initial_path_taps": [
    [
      0,
      1.7
    ],
    [
      17,
      -0.22
    ],
    [
      432,
      0.34
    ],
    [
      912,
      0.16
    ]
  ],
  "changed_path_taps": [
    [
      0,
      1.25
    ],
    [
      17,
      0.85
    ],
    [
      432,
      0.34
    ],
    [
      912,
      0.16
    ]
  ],
  "path_change_seconds": 6,
  "after_change_gain_magnitude": 0.55,
  "scenario_notes": "control retains initial taps; path_change replaces taps; polarity_and_path_change additionally negates gain; gap variant drops a capture packet at eight seconds",
  "near_start_seconds": 10,
  "echo_end_seconds": 14,
  "initial_far_window": [
    192000,
    288000
  ],
  "after_change_far_window": [
    432000,
    480000
  ],
  "double_talk_window": [
    576000,
    672000
  ],
  "near_only_window": [
    720000,
    768000
  ],
  "metric_basis": "pre-PCM16 float samples, ERLE on echo-only windows, projection and residual error on separate near speech windows",
  "limitations": "Synthetic development path, physical headset attenuation and perceptual voice retention remain unverified"
})JSON";
    require(static_cast<bool>(report) && static_cast<bool>(manifest),
            "cannot write field evidence report");
  }
  // At field levels a valid alignment must produce audible useful attenuation,
  // including after a gain change and any short reference interruption.
  require(initial.erle() > 5 && changed.erle() > 5 &&
              changed.changed > changed.samples / 2,
          "field-level aligned filter failed useful echo subtraction");
  require(overlap.nearCross / overlap.nearEnergy > 0.90 &&
              overlap.nearCross / overlap.nearEnergy < 1.10 &&
              overlap.errorEnergy / overlap.nearEnergy < 0.15,
          "field-level cancellation damaged simultaneous near speech");
  require(nearOnly.nearCross / nearOnly.nearEnergy > 0.98 &&
              nearOnly.errorEnergy / nearOnly.nearEnergy < 0.005,
          "field-level echo disappearance damaged independent speech");
}

} // namespace

int main() {
  constexpr std::array<std::array<bool, 3>, 4> cases{{
      {false, false, false},
      {true, true, false},
      {true, false, false},
      {true, false, true},
  }};
  bool passed = true;
  for (const auto &scenario : cases) {
    try {
      fieldLevelPostAlignment(scenario[0], scenario[1], scenario[2]);
    } catch (const std::exception &error) {
      std::cerr << "post alignment: " << error.what() << '\n';
      passed = false;
    }
  }
  if (passed) {
    std::cout << "post-alignment integration tests passed\n";
  }
  return passed ? 0 : 1;
}
