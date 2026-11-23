#include "aec.hpp"
#include "diagnostics.hpp"
#include "host_clock.hpp"
#include "reference_timeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int rate = 48000;
constexpr int acousticDelay = 5760;
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

struct Delivery {
  buncancel::capture::ReferencePacket packet;
  double arrivalFrame{};
};

std::vector<Delivery> packets(const Stimulus &render, int frames, double ppm,
                              bool jitter, bool loss, int preroll) {
  std::vector<Delivery> result;
  constexpr std::array<int, 5> sizes{480, 240, 360, 480, 120};
  int native = -preroll;
  const double ratio = 1 + ppm * 1.0e-6;
  for (int sequence = 0; native < frames * ratio; ++sequence) {
    const int count = jitter ? sizes[sequence % sizes.size()] : 480;
    Delivery delivery;
    auto &packet = delivery.packet;
    packet.frame_count = count;
    packet.channel_count = 2;
    packet.sample_rate = rate;
    packet.device_position = 9000000ULL + native;
    packet.qpc_position_100ns = stamp(native / ratio);
    packet.generation = 7;
    for (int i = 0; i < count; ++i) {
      const float value = render.sample((native + i) / ratio);
      packet.samples[2 * i] = value;
      packet.samples[2 * i + 1] = value;
    }
    delivery.arrivalFrame = (native + count) / ratio + 48;
    if (jitter) {
      delivery.arrivalFrame += (sequence % 4) * 32;
    }
    // Real missing capture data, rather than a late packet masquerading as
    // loss.
    if (!loss || native < 3 * rate || native >= 3 * rate + rate / 4) {
      result.push_back(delivery);
    }
    native += count;
  }
  return result;
}

struct Metrics {
  double erle{};
  double nearGain{};
  double nearErrorRatio{};
  double callbackMaxUs{};
  int availableBlocks{};
  int missingBlocks{};
  int resetBlocks{};
  int finalDelay{};
  bool finalAligned{};
  double recoveryMs{};
};

struct Scenario {
  const char *name;
  bool overlap{};
  bool independent{};
  bool silentMic{};
  bool jitter{};
  bool loss{};
  double driftPpm{};
  int block{256};
  bool silentReference{};
  bool changingEcho{};
  int pathDelay{acousticDelay};
  int prerollFrames{};
  bool speechLike{};
  bool muteAfterTraining{};
  bool callbackJitter{};
};

Metrics run(const Scenario &scenario, bool oldFifo) {
  constexpr int frames = 6 * rate;
  const Stimulus render(frames + rate, 197, scenario.speechLike);
  const Stimulus near(frames + rate, 991, scenario.speechLike);
  const auto deliveries =
      packets(render, frames, scenario.driftPpm, scenario.jitter, scenario.loss,
              scenario.prerollFrames);
  buncancel::capture::ReferenceTimelineConfig config;
  config.sample_rate = rate;
  config.max_block_frames = 2048;
  config.holdback_frames = 960;
  buncancel::capture::ReferenceTimeline timeline(config);
  buncancel::Aec engine(rate);
  buncancel::plugin::HostClock hostClock(rate);
  std::array<float, 2048> mic{}, reference{}, output{}, desired{};
  std::size_t delivered = 0;
  std::size_t fifoPacket = 0;
  unsigned fifoOffset = 0;
  Metrics metrics;
  double inputEnergy = 0;
  double outputEnergy = 0;
  double nearEnergy = 0;
  double nearCross = 0;
  double nearError = 0;
  bool measuredLossDry = false;
  int readyBlocks = 0;
  int reducingBlocks = 0;
  bool previousUsable = true;
  for (int start = 0; start < frames; start += scenario.block) {
    const int count = std::min(scenario.block, frames - start);
    const int schedulingJitter =
        scenario.callbackJitter ? ((start / scenario.block) % 5 - 2) * 24 : 0;
    const double callbackFrame = start + count + schedulingJitter;
    const auto clock =
        hostClock.observe(stamp(callbackFrame), count, true, start);
    if (clock.reset_required) {
      timeline.reset();
    }
    while (delivered < deliveries.size() &&
           deliveries[delivered].arrivalFrame <= callbackFrame) {
      if (!oldFifo) {
        auto packet = deliveries[delivered].packet;
        if (scenario.silentReference) {
          packet.samples.fill(0);
        }
        timeline.push(packet);
      }
      ++delivered;
    }
    for (int i = 0; i < count; ++i) {
      const int time = start + i;
      desired[i] =
          (scenario.independent || (scenario.overlap && time >= 2 * rate))
              ? near.sample(time)
              : 0;
      mic[i] = scenario.silentMic ? 0 : desired[i];
      const bool echoPresent =
          !scenario.changingEcho ||
          (time < 2 * rate || (time >= 3 * rate && time < 5 * rate));
      if (!scenario.independent && !scenario.silentMic && echoPresent) {
        mic[i] += 0.85F * render.sample(time - scenario.pathDelay) +
                  0.10F * render.sample(time - scenario.pathDelay - 23);
      }
    }
    if (scenario.muteAfterTraining && start >= 2 * rate) {
      std::fill_n(mic.data(), count, 0.0F);
    }
    const auto begin = std::chrono::steady_clock::now();
    bool usable = true;
    if (oldFifo) {
      // Reproduce the former plugin consumer: consume partial packets until an
      // empty queue, discard the partial callback, and pass nullptr to Aec.
      // Aec then resets all delay/adaptation history on each starvation event.
      for (int i = 0; i < count; ++i) {
        if (fifoPacket >= delivered) {
          usable = false;
          break;
        }
        const auto &packet = deliveries[fifoPacket].packet;
        if (stamp(start) > packet.qpc_position_100ns &&
            stamp(start) - packet.qpc_position_100ns > 1000000) {
          ++fifoPacket;
          fifoOffset = 0;
          usable = false;
          break;
        }
        reference[i] = packet.samples[2 * fifoOffset++];
        if (fifoOffset >= packet.frame_count) {
          ++fifoPacket;
          fifoOffset = 0;
        }
      }
    } else {
      const auto result = timeline.render(clock.microphone_start_qpc_100ns,
                                          count, reference.data());
      usable = result.usable;
      if (result.reset_required) {
        engine.reset();
        ++metrics.resetBlocks;
      }
    }
    if (oldFifo && !usable && previousUsable) {
      ++metrics.resetBlocks;
    }
    previousUsable = usable;
    engine.process(mic.data(), usable ? reference.data() : nullptr,
                   output.data(), count);
    const auto end = std::chrono::steady_clock::now();
    if (!oldFifo) {
      const auto diagnostic = engine.diagnostics();
      buncancel::plugin::ProcessingObservation observation;
      observation.enabled = true;
      observation.selected = true;
      observation.connected = true;
      observation.processing = usable;
      observation.reducing = engine.aligned() && diagnostic.reductionValid &&
                             diagnostic.reductionDb > 0.1F;
      const auto status =
          buncancel::plugin::selectProcessingStatus(observation);
      if (usable && !observation.reducing) {
        require(status == buncancel::plugin::ProcessingStatus::readyMonitoring,
                "compatible signal stream was not ready while monitoring");
        ++readyBlocks;
      } else if (usable) {
        require(status == buncancel::plugin::ProcessingStatus::cancelling,
                "measured subtraction was not distinguished from readiness");
        ++reducingBlocks;
      }
    }
    metrics.callbackMaxUs = std::max(
        metrics.callbackMaxUs,
        std::chrono::duration<double, std::micro>(end - begin).count());
    usable ? ++metrics.availableBlocks : ++metrics.missingBlocks;
    if (scenario.loss && start > 3 * rate + rate / 4 && engine.aligned() &&
        metrics.recoveryMs == 0) {
      metrics.recoveryMs = (start - (3 * rate + rate / 4)) * 1000.0 / rate;
    }
    for (int i = 0; i < count; ++i) {
      require(std::isfinite(output[i]), "pipeline emitted nonfinite audio");
      if (scenario.silentMic ||
          (scenario.muteAfterTraining && start >= 2 * rate)) {
        require(output[i] == 0, "reference bled into silent microphone");
      }
      if (!usable) {
        require(output[i] == mic[i],
                "reference loss failed exact dry fallback");
        measuredLossDry |= start > 3 * rate && start < 3 * rate + rate / 4;
      }
      if (start >= (scenario.loss ? 5 : 4) * rate) {
        inputEnergy += static_cast<double>(mic[i]) * mic[i];
        outputEnergy += static_cast<double>(output[i]) * output[i];
        nearEnergy += static_cast<double>(desired[i]) * desired[i];
        nearCross += static_cast<double>(output[i]) * desired[i];
        const double error = output[i] - desired[i];
        nearError += error * error;
      }
    }
  }
  metrics.erle =
      10 * std::log10((inputEnergy + 1.0e-30) / (outputEnergy + 1.0e-30));
  metrics.nearGain = nearCross / (nearEnergy + 1.0e-30);
  metrics.nearErrorRatio = nearError / (nearEnergy + 1.0e-30);
  metrics.finalDelay = engine.delaySamples();
  metrics.finalAligned = engine.aligned();
  if (scenario.loss && !oldFifo) {
    require(measuredLossDry, "loss fixture did not exercise missing reference");
  }
  std::cout << scenario.name << (oldFifo ? " old_fifo" : " timeline")
            << " available=" << metrics.availableBlocks
            << " missing=" << metrics.missingBlocks
            << " resets=" << metrics.resetBlocks
            << " aligned=" << metrics.finalAligned
            << " delay_samples=" << metrics.finalDelay;
  // ERLE is meaningful only when the microphone contains echo alone.
  if (!scenario.overlap && !scenario.independent && !scenario.silentMic &&
      !scenario.muteAfterTraining) {
    std::cout << " far_only_erle_db=" << metrics.erle;
  }
  if (scenario.overlap || scenario.independent) {
    std::cout << " near_projection_gain=" << metrics.nearGain
              << " near_error_energy_ratio=" << metrics.nearErrorRatio;
  }
  std::cout << " callback_max_us=" << metrics.callbackMaxUs
            << " ready_blocks=" << readyBlocks
            << " reducing_blocks=" << reducingBlocks
            << " recovery_ms=" << metrics.recoveryMs
            << " callback_deadline_us=" << scenario.block * 1.0e6 / rate
            << '\n';
  return metrics;
}
void hostClockContract() {
  buncancel::plugin::HostClock clock(rate);
  constexpr std::array<int, 5> blocks{64, 127, 256, 512, 2048};
  int frame = 0;
  int resets = 0;
  double maxErrorTicks = 0;
  std::uint64_t previous = 0;
  for (int block = 0; block < 1500; ++block) {
    const int count = blocks[block % blocks.size()];
    // Independent mic clock runs 80ppm fast. QPC scheduling noise is bounded
    // +/-0.5ms, while the arbitrary host sample position has a separate epoch.
    const double physicalStart = frame / 1.00008;
    const double physicalEnd = (frame + count) / 1.00008;
    const auto callback =
        stamp(physicalEnd) + static_cast<std::int64_t>((block % 5 - 2) * 2500);
    const auto result = clock.observe(callback, count, true, frame + 1234567);
    resets += result.reset_required;
    const double error =
        std::abs(static_cast<double>(result.microphone_start_qpc_100ns) -
                 static_cast<double>(stamp(physicalStart)));
    maxErrorTicks = std::max(maxErrorTicks, error);
    require(block == 0 || result.microphone_start_qpc_100ns > previous,
            "host clock moved backwards under scheduling jitter");
    previous = result.microphone_start_qpc_100ns;
    frame += count;
  }
  require(resets == 1 && maxErrorTicks < 10000,
          "host clock lost continuity under block changes and slow drift");
  const auto pause =
      clock.observe(stamp(frame + rate), 256, true, frame + 1234567);
  require(pause.reset_required, "host clock did not reanchor after long pause");
  const auto seek = clock.observe(stamp(frame + rate + 256), 256, true, 0);
  require(seek.reset_required,
          "host sample discontinuity did not reset timing");
  std::cout
      << "host_clock variable_blocks=1500 drift_ppm=80 scheduling_jitter_ms=0.5"
      << " max_start_error_us=" << maxErrorTicks / 10
      << " pause_and_seek_resets=2\n";
}
} // namespace

int main() {
  try {
    hostClockContract();
    const Scenario baseline{"slow_capture_fifo_reproducer",
                            false,
                            false,
                            false,
                            true,
                            false,
                            -1000};
    const auto former = run(baseline, true);
    const auto current = run(baseline, false);
    require(former.missingBlocks > 10 && former.erle < 10,
            "former FIFO starvation reproducer failed");
    require(current.erle > 15 && current.finalAligned,
            "timestamped reference did not converge");
    require(current.erle > former.erle + 15,
            "timeline did not improve former FIFO reproducer");
    require(std::abs(current.finalDelay - (acousticDelay - 960)) <= 2,
            "timeline holdback was not included in effective acoustic delay");
    const Scenario ordinary{"ordinary_fifo_control"};
    require(run(ordinary, true).erle > 15,
            "ordinary packet mismatch alone unexpectedly prevented old "
            "convergence");
    require(run(ordinary, false).erle > 15,
            "ordinary timestamp reference failed convergence");
    Scenario backlog{"startup_backlog_short_path"};
    backlog.prerollFrames = 3840;
    backlog.pathDelay = 1920;
    const auto oldBacklog = run(backlog, true);
    const auto newBacklog = run(backlog, false);
    require(!oldBacklog.finalAligned && std::abs(oldBacklog.erle) < 0.01 &&
                oldBacklog.missingBlocks == 0,
            "80ms startup backlog did not reproduce negative FIFO delay");
    require(
        newBacklog.finalAligned && newBacklog.erle > 15 &&
            std::abs(newBacklog.finalDelay - 960) <= 2,
        "timestamp timeline did not recover short path with startup backlog");
    const auto overlap = run({"loud_render_overlap", true}, false);
    require(overlap.nearGain > 0.95 && overlap.nearGain < 1.05 &&
                overlap.nearErrorRatio < 0.12,
            "double-talk damaged independent near-end signal");
    const auto independent =
        run({"independent_near_reference", false, true}, false);
    require(independent.nearErrorRatio < 1.0e-12,
            "uncorrelated reference changed independent microphone");
    run({"silent_microphone", false, false, true}, false);
    Scenario silentReference{"silent_reference_live_near", false, true};
    silentReference.silentReference = true;
    require(run(silentReference, false).nearErrorRatio < 1.0e-12,
            "silent reference modified live near-end microphone");
    Scenario changing{"abrupt_echo_on_off", true};
    changing.changingEcho = true;
    require(run(changing, false).nearGain > 0.95,
            "echo appearance/disappearance attenuated near-end microphone");
    Scenario shortPath{"short_path_safe_dry"};
    shortPath.pathDelay = 731;
    const auto shortResult = run(shortPath, false);
    require(!shortResult.finalAligned && std::abs(shortResult.erle) < 0.01,
            "short path requiring reference lookahead did not stay dry");
    Scenario overrange{"overrange_delay_safe_dry"};
    overrange.pathDelay = 20000;
    const auto distant = run(overrange, false);
    require(!distant.finalAligned && std::abs(distant.erle) < 0.01,
            "unsupported long echo path did not stay dry");
    Scenario trainedMute{"trained_filter_unflagged_mute"};
    trainedMute.muteAfterTraining = true;
    run(trainedMute, false);
    Scenario speechFar{"speech_like_far_only"};
    speechFar.speechLike = true;
    require(run(speechFar, false).erle > 15,
            "speech-like far-only signal failed cancellation");
    Scenario speechOverlap{"speech_like_double_talk", true};
    speechOverlap.speechLike = true;
    const auto speech = run(speechOverlap, false);
    require(speech.nearGain > 0.95 && speech.nearGain < 1.05,
            "speech-like double-talk attenuated near-end component");
    const auto varied =
        run({"packet_jitter", false, false, false, true}, false);
    require(varied.erle > 15, "packet size/jitter prevented convergence");
    const auto drift =
        run({"clock_drift", false, false, false, true, false, 150}, false);
    require(drift.erle > 10, "timestamp drift compensation failed");
    Scenario scheduled{"callback_qpc_jitter"};
    scheduled.callbackJitter = true;
    require(
        run(scheduled, false).erle > 10,
        "production host clock did not tolerate callback scheduling jitter");
    const auto recovery = run(
        {"reference_loss_recovery", false, false, false, true, true}, false);
    require(recovery.erle > 15 && recovery.finalAligned &&
                recovery.recoveryMs < 1500,
            "reference loss failed reacquisition");
    for (const int block : {64, 127, 512, 2048}) {
      Scenario partition{"host_block_matrix"};
      partition.block = block;
      require(run(partition, false).erle > 15,
              "host callback partition prevented convergence");
    }
    std::cout << "reference pipeline synthetic integration tests passed; "
                 "physical device/host cause remains unverified\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "reference pipeline: " << error.what() << '\n';
    return 1;
  }
}
