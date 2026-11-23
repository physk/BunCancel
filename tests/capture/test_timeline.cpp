#include "reference_timeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

namespace {
std::atomic<unsigned> allocations{};
int failures{};
constexpr std::uint64_t epoch = 216000000000ULL;

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}

std::uint64_t timestamp(double frame, double rate = 48000) {
  return epoch +
         static_cast<std::uint64_t>(std::llround(frame * 10000000 / rate));
}

buncancel::capture::ReferencePacket packet(std::uint64_t position,
                                           std::uint32_t count = 240,
                                           double clock_rate = 48000,
                                           std::uint32_t nominal_rate = 48000) {
  buncancel::capture::ReferencePacket result;
  result.frame_count = count;
  result.channel_count = 2;
  result.sample_rate = nominal_rate;
  result.device_position = position;
  result.qpc_position_100ns =
      timestamp(static_cast<double>(position), clock_rate);
  result.generation = 1;
  for (unsigned i = 0; i < count; ++i) {
    // A bounded linear ramp allows exact testing of interpolation.
    const float sample = static_cast<float>(position + i) / 100000;
    result.samples[i * 2] = sample + 0.1F;
    result.samples[i * 2 + 1] = sample - 0.1F;
  }
  return result;
}

void timestamp_mapping_and_delivery_jitter() {
  using namespace buncancel::capture;
  ReferenceTimeline timeline({48000, 240, 4096, 960});
  std::array<float, 240> output{};
  for (unsigned i = 0; i < 8; ++i) {
    check(timeline.push(packet(i * 240)),
          "initial timestamped packet accepted");
  }
  auto result = timeline.render(timestamp(1440), output.size(), output.data());
  check(result.usable && result.reset_required &&
            result.reference_delay_frames == 960,
        "startup uses QPC epoch and exposes holdback without mic lookahead");
  check(std::abs(output.front() - 0.0048F) < 0.000001F &&
            std::abs(output.back() - 0.00719F) < 0.000001F,
        "stereo mix is interpolated at mic time minus explicit holdback");
  check(result.rms > 0 && result.peak >= result.rms,
        "ready block reports reference energy");
  // Worker delivers two packets together, then no packet for a callback. Only
  // their source timestamps, rather than the pop schedule, determine audio.
  check(timeline.push(packet(1920)) && timeline.push(packet(2160)),
        "bursty packet delivery accepted");
  for (unsigned mic = 1680; mic <= 2160; mic += 240) {
    result = timeline.render(timestamp(mic), output.size(), output.data());
    check(result.usable && !result.reset_required,
          "bounded delivery jitter must not restart reference history");
    check(std::abs(output.front() - static_cast<float>(mic - 960) / 100000) <
              0.000001F,
          "delivery jitter must not retime reference");
  }
}

void drift_and_sample_rate_conversion() {
  using namespace buncancel::capture;
  for (const double drift : {-750.0, 750.0}) {
    ReferenceTimeline timeline({48000, 128, 8192, 0});
    const double source_rate = 48000 * (1 + drift / 1000000);
    for (unsigned i = 0; i < 16; ++i) {
      check(timeline.push(packet(i * 240, 240, source_rate)),
            "bounded device drift must be accepted");
    }
    std::array<float, 128> output{};
    const auto result =
        timeline.render(timestamp(2500), output.size(), output.data());
    check(result.usable && std::abs(result.source_rate_ppm - drift) < 1,
          "device-position and QPC deltas measure clock drift");
    for (unsigned i = 0; i < output.size(); ++i) {
      const float expected =
          static_cast<float>((2500.0 + i) * source_rate / 48000 / 100000);
      check(std::abs(output[i] - expected) < 0.000001F,
            "clock drift must be interpolated onto microphone clock");
    }
  }
  ReferenceTimeline timeline({48000, 128, 8192, 0});
  for (unsigned i = 0; i < 10; ++i) {
    check(timeline.push(packet(i * 240, 240, 44100, 44100)),
          "ordinary reference sample-rate conversion accepted");
  }
  std::array<float, 128> output{};
  const auto result =
      timeline.render(timestamp(1000), output.size(), output.data());
  check(result.usable && std::abs(output.front() - 0.0091875F) < 0.000001F,
        "44.1 kHz reference is resampled at 48 kHz mic timestamps");
}

void noisy_annotations_and_sustained_bad_clock() {
  using namespace buncancel::capture;
  ReferenceTimeline timeline({48000, 128, 8192, 960});
  std::array<float, 128> output{};
  for (unsigned i = 0; i < 300; ++i) {
    auto p = packet(i * 240);
    // Keep waveform bounded for this longer clock test.
    p.samples.fill(0.25F);
    const auto noise = static_cast<std::int64_t>(
        std::llround(std::sin(static_cast<double>(i) * 1.7) * 1000));
    p.qpc_position_100ns = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(p.qpc_position_100ns) + noise);
    check(timeline.push(p),
          "100 us QPC annotation noise must not be confused with clock loss");
    if (i > 8) {
      const auto result =
          timeline.render(timestamp(i * 240), output.size(), output.data());
      check(result.usable && (i == 9 || !result.reset_required),
            "noisy timestamp fit preserves continuous reference coverage");
      check(std::abs(output.front() - 0.25F) < 0.000001F,
            "timestamp smoothing must preserve bounded source samples");
    }
  }
  timeline.reset();
  bool rejected{};
  for (unsigned i = 0; i < 300 && !rejected; ++i) {
    auto p = packet(i * 240, 240, 48000 * 1.004);
    p.samples.fill(0.25F);
    rejected = !timeline.push(p);
  }
  const auto result =
      timeline.render(timestamp(2000), output.size(), output.data());
  check(rejected && !result.usable &&
            result.status == TimelineStatus::clock_mismatch,
        "sustained clock-rate error outside configured range must fail dry");
}

void epochs_duplicates_gaps_and_guards() {
  using namespace buncancel::capture;
  ReferenceTimeline timeline({48000, 128, 4096, 0});
  std::array<float, 128> output{};
  auto result = timeline.render(timestamp(0), output.size(), output.data());
  check(!result.usable && result.status == TimelineStatus::empty,
        "startup without reference fails dry");
  auto first = packet(0);
  check(timeline.push(first) && timeline.push(packet(240)),
        "initial stream established");
  result = timeline.render(timestamp(100), output.size(), output.data());
  check(result.usable, "initial covered block available");
  check(!timeline.push(first), "duplicate rejected");
  result = timeline.render(timestamp(200), output.size(), output.data());
  check(result.usable && !result.reset_required,
        "duplicate must not flush valid history");
  result = timeline.render(timestamp(400), output.size(), output.data());
  check(!result.usable && result.reset_required &&
            result.status == TimelineStatus::underrun &&
            std::all_of(output.begin(), output.end(),
                        [](float x) { return x == 0; }),
        "partial availability fails whole block dry without extrapolation");
  check(timeline.push(packet(960)), "gap begins new bounded timeline");
  result = timeline.render(timestamp(960), output.size(), output.data());
  check(result.usable && result.reset_required,
        "missing device positions force adaptation reset before recovery");
  auto restart = packet(0);
  restart.discontinuity = true;
  check(timeline.push(restart),
        "explicit discontinuity permits same-generation position restart");
  result = timeline.render(timestamp(0), output.size(), output.data());
  check(result.usable && result.reset_required,
        "duplicate filter must honor explicit capture discontinuity");
  restart.generation = 2;
  check(timeline.push(restart),
        "new generation permits device-position restart");
  check(!timeline.push(packet(1200)),
        "old generation must not revive stale audio");
  result = timeline.render(timestamp(0), output.size(), output.data());
  check(result.usable && result.reset_required,
        "startup epoch is QPC rather than device position or queue age");
  auto bad_clock = packet(240);
  bad_clock.generation = 2;
  bad_clock.qpc_position_100ns += 100000;
  check(!timeline.push(bad_clock), "large clock jumps rejected");
  result = timeline.render(timestamp(100), output.size(), output.data());
  check(!result.usable && result.reset_required &&
            result.status == TimelineStatus::clock_mismatch,
        "inconsistent clock must flush history and fail dry");
  auto invalid = packet(0);
  invalid.samples[0] = std::numeric_limits<float>::quiet_NaN();
  check(!timeline.push(invalid), "nonfinite samples rejected");
  result = timeline.render(timestamp(0), output.size(), output.data());
  check(!result.usable && result.status == TimelineStatus::invalid_packet,
        "invalid packet cannot yield usable reference");
}

void large_offsets_and_ring_wrap() {
  using namespace buncancel::capture;
  ReferenceTimeline timeline({48000, 128, 1024, 0});
  constexpr std::uint64_t huge_epoch =
      std::numeric_limits<std::uint64_t>::max() - 10000000;
  constexpr std::uint64_t device_epoch = 10000000000000000ULL;
  for (unsigned i = 0; i < 12; ++i) {
    auto p = packet(i * 240);
    p.device_position += device_epoch;
    p.qpc_position_100ns = huge_epoch + timestamp(i * 240) - epoch;
    check(timeline.push(p), "large clock and device-position epochs accepted");
  }
  std::array<float, 128> output{};
  auto result = timeline.render(huge_epoch + timestamp(2500) - epoch,
                                output.size(), output.data());
  check(result.usable && std::abs(output.front() - 0.025F) < 0.000001F,
        "large epochs and ring wrap preserve interpolation precision");
  result = timeline.render(huge_epoch, output.size(), output.data());
  check(!result.usable, "overwritten old timestamps fail dry");
}

void realtime_storage() {
  using namespace buncancel::capture;
  ReferenceTimeline timeline({48000, 128, 4096, 0});
  std::array<float, 128> output{};
  const auto before = allocations.load();
  for (unsigned i = 0; i < 12; ++i) {
    const auto p = packet(i * 240);
    const bool pushed = timeline.push(p);
    const auto result =
        timeline.render(timestamp(i * 240), output.size(), output.data());
    if (!pushed || !result.usable) {
      ++failures;
    }
  }
  timeline.reset();
  check(before == allocations.load(), "push/render/reset must never allocate");
}
} // namespace

void *operator new(std::size_t size) {
  allocations.fetch_add(1);
  if (void *memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

int main() {
  timestamp_mapping_and_delivery_jitter();
  drift_and_sample_rate_conversion();
  noisy_annotations_and_sustained_bad_clock();
  epochs_duplicates_gaps_and_guards();
  large_offsets_and_ring_wrap();
  realtime_storage();
  if (failures != 0) {
    std::cerr << failures << " reference timeline failures\n";
    return 1;
  }
  std::cout
      << "Reference timestamp, jitter, drift and fail-dry checks passed\n";
}
