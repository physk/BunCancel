#include "packet_queue.hpp"
#include "reference_capture.hpp"
#include "reference_fragment.hpp"
#include "sample_format.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>

namespace {
int failures{};

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}

void queue_boundaries() {
  buncancel::capture::detail::PacketQueue<unsigned, 3> queue;
  unsigned value{};
  check(!queue.pop(value), "empty queue must not yield data");
  for (unsigned cycle = 0; cycle < 100; ++cycle) {
    check(queue.push(cycle * 3), "first slot must accept data");
    check(queue.push(cycle * 3 + 1), "second slot must accept data");
    check(queue.push(cycle * 3 + 2), "last slot must accept data");
    check(!queue.push(9999), "full queue must reject incoming data");
    for (unsigned index = 0; index < 3; ++index) {
      check(queue.pop(value) && value == cycle * 3 + index,
            "queue must preserve order after overflow and wrap");
    }
    check(!queue.pop(value), "drained queue must be empty");
  }
}

void queue_concurrency() {
  buncancel::capture::detail::PacketQueue<std::array<unsigned, 4>, 7> queue;
  constexpr unsigned count = 100000;
  std::atomic<bool> producer_finished{};
  std::thread producer([&] {
    for (unsigned index = 0; index < count; ++index) {
      const std::array<unsigned, 4> packet{index, index ^ 0x55AAU, index + 1,
                                           index + 2};
      while (!queue.push(packet)) {
        std::this_thread::yield();
      }
    }
    producer_finished.store(true, std::memory_order_release);
  });
  unsigned expected{};
  while (expected < count) {
    std::array<unsigned, 4> packet{};
    if (queue.pop(packet)) {
      check(packet[0] == expected && packet[1] == (expected ^ 0x55AAU) &&
                packet[2] == expected + 1 && packet[3] == expected + 2,
            "concurrent packet must be whole and ordered");
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  check(producer_finished.load(std::memory_order_acquire),
        "producer must complete all packets");
}

void sample_formats() {
  using namespace buncancel::capture::detail;
  SampleFormat format;
  check(valid_format(format), "float stereo 48 kHz must be supported");
  format.channels = 6;
  check(!valid_format(format),
        "surround must fail instead of dropping channels");
  format.channels = 2;
  format.sample_rate = 0;
  check(!valid_format(format), "zero sample rate must fail");
  format.sample_rate = 48000;
  format.bits = 64;
  check(!valid_format(format), "float64 must fail");
  format.bits = 32;
  for (const float source :
       {0.25F, -0.75F, 2.0F, -2.0F, std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()}) {
    std::array<std::uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), &source, sizeof(source));
    const auto result = decode_sample(bytes.data(), format);
    check(std::isfinite(result) && std::abs(result) <= 1.0F,
          "nonfinite/overrange float must not propagate");
    if (std::isfinite(source) && std::abs(source) <= 1.0F) {
      check(result == source, "ordinary float must remain exact");
    }
  }
  format.encoding = Encoding::signed_pcm;
  for (const auto bits : {16, 24, 32}) {
    format.bits = static_cast<std::uint16_t>(bits);
    format.valid_bits = format.bits;
    check(valid_format(format), "PCM 16/24/32 must be supported");
    std::array<std::uint8_t, 4> minimum{};
    minimum[bits / 8 - 1] = 0x80;
    check(decode_sample(minimum.data(), format) == -1.0F,
          "PCM minimum must decode as negative full scale");
    std::array<std::uint8_t, 4> half{};
    half[bits / 8 - 1] = 0x40;
    check(decode_sample(half.data(), format) == 0.5F,
          "PCM half scale must be independent of container width");
  }
  format.valid_bits = 24;
  const std::array<std::uint8_t, 4> left_aligned{0, 0, 0, 0x40};
  check(valid_format(format) &&
            decode_sample(left_aligned.data(), format) == 0.5F,
        "24 valid bits in 32-bit PCM must preserve left alignment");
  format.valid_bits = 33;
  check(!valid_format(format), "valid bits cannot exceed container width");
}

void timestamp_error_fragments() {
  using namespace buncancel::capture;
  using namespace buncancel::capture::detail;
  SampleFormat format;
  format.encoding = Encoding::signed_pcm;
  format.bits = format.valid_bits = 16;
  format.channels = 2;
  constexpr std::uint32_t frames = ReferencePacket::max_frames + 19;
  std::array<std::uint8_t, frames * 4> pcm{};
  for (std::uint32_t i = 0; i < frames; ++i) {
    pcm[i * 4 + 1] = 0x40; // Left +0.5.
    pcm[i * 4 + 3] = 0xC0; // Right -0.5.
  }
  bool discontinuity = false;
  for (std::uint32_t offset = 0; offset < frames;) {
    const auto packet =
        reference_fragment(pcm.data(), format, frames, offset, 1000, 12345678,
                           7, false, false, discontinuity);
    check(packet.qpc_position_100ns == 0 && packet.discontinuity &&
              discontinuity,
          "timestamp errors must remain unusable at every fragment offset");
    check(packet.device_position == 1000 + offset && packet.generation == 7,
          "timestamp errors must preserve source positions and generation");
    for (std::uint32_t i = 0; i < packet.frame_count; ++i) {
      check(packet.samples[2 * i] == 0.5F && packet.samples[2 * i + 1] == -0.5F,
            "timestamp errors must preserve PCM for raw source metering");
    }
    offset += packet.frame_count;
  }
  const auto recovery =
      reference_fragment(pcm.data(), format, frames, 512, 2000, 23456789, 7,
                         false, true, discontinuity);
  check(
      recovery.discontinuity && !discontinuity &&
          recovery.qpc_position_100ns == 23456789 + 512ULL * 10000000 / 48000,
      "first valid timestamp after an error must reset and retain offset time");
  const auto normal =
      reference_fragment(pcm.data(), format, frames, 0, 3000, 34567890, 7,
                         false, true, discontinuity);
  check(!normal.discontinuity && !discontinuity,
        "successful timestamp recovery must permit subsequent continuous data");
  const auto silent =
      reference_fragment(nullptr, format, frames, 512, 4000, 45678901, 7, true,
                         false, discontinuity);
  check(
      silent.qpc_position_100ns == 0 && silent.samples[0] == 0 &&
          silent.discontinuity && discontinuity,
      "silent timestamp-error fragments must remain explicit zero-QPC silence");
}

void lifecycle_contract() {
  using namespace buncancel::capture;
  ReferenceCapture capture;
  ReferencePacket packet;
  check(capture.status() == CaptureStatus::stopped && !capture.pop(packet),
        "new capture must be dry");
  check(!capture.start({}), "empty endpoint must be rejected");
  check(capture.status() == CaptureStatus::device_error && !capture.pop(packet),
        "invalid configuration must remain dry");
  capture.stop();
  capture.stop();
  check(capture.status() == CaptureStatus::stopped,
        "repeated stop must be harmless");
  check(capture.dropped_packets() == 0,
        "invalid startup must not invent dropped audio");
#ifndef _WIN32
  check(ReferenceCapture::enumerate_endpoints().empty(),
        "non-Windows builds must not invent endpoints");
  check(!capture.start({"unavailable", EndpointKind::render_loopback, 48000}) &&
            !capture.pop(packet),
        "non-Windows capture must fail dry");
#endif
}
} // namespace

int main() {
  queue_boundaries();
  queue_concurrency();
  sample_formats();
  timestamp_error_fragments();
  lifecycle_contract();
  if (failures != 0) {
    std::cerr << failures << " capture contract failures\n";
    return 1;
  }
  std::cout << "Capture queue, sample format and lifecycle checks passed\n";
  return 0;
}
