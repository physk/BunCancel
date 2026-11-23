#include "local_capture.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
thread_local bool tracking = false;
thread_local std::size_t allocations = 0;
} // namespace

#ifdef _MSC_VER
#define BUNCANCEL_NOINLINE __declspec(noinline)
#else
#define BUNCANCEL_NOINLINE __attribute__((noinline))
#endif

BUNCANCEL_NOINLINE void *operator new(std::size_t size) {
  if (tracking)
    ++allocations;
  if (void *memory = std::malloc(size ? size : 1))
    return memory;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
BUNCANCEL_NOINLINE void operator delete(void *memory) noexcept {
  std::free(memory);
}
BUNCANCEL_NOINLINE void operator delete[](void *memory) noexcept {
  std::free(memory);
}
BUNCANCEL_NOINLINE void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}
BUNCANCEL_NOINLINE void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

namespace {
using namespace buncancel::plugin;
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::string read_text(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
std::uint32_t le(const std::string &bytes, std::size_t position, int count) {
  std::uint32_t value = 0;
  for (int i = 0; i < count; ++i)
    value |= static_cast<unsigned char>(bytes[position + i]) << (8 * i);
  return value;
}
class CommaDecimal final : public std::numpunct<char> {
  char do_decimal_point() const override { return ','; }
};

void exact_session(const std::filesystem::path &parent) {
  LocalCapture capture;
  std::array<float, 2048> mic{}, reference{}, output{};
  for (std::size_t i = 0; i < mic.size(); ++i) {
    mic[i] = static_cast<float>(static_cast<int>(i % 23) - 11) / 1024;
    reference[i] = mic[i] * 0.5F;
    output[i] = mic[i] * 0.25F;
  }
  LocalCaptureBlock info;
  info.diagnostics.changed_samples = 999999;
  capture.append(mic.data(), reference.data(), output.data(), 512, info);
  check(capture.captured_frames() == 0 &&
            capture.state() == LocalCaptureState::idle,
        "capture retained audio before explicit arm");
  check(capture.arm(parent, 8000), "cannot arm local capture");
  check(!capture.arm(parent, 8000), "capture rearmed while audio owner active");
  const auto saved_locale = std::locale();
  std::locale::global(std::locale(saved_locale, new CommaDecimal));
  std::uint32_t offset = 0;
  allocations = 0;
  tracking = true;
  while (capture.state() == LocalCaptureState::recording) {
    info.microphone_qpc_100ns =
        offset == 0 ? 0 : 654321987000ULL + offset * 1250;
    info.reference_qpc_100ns =
        info.microphone_qpc_100ns ? info.microphone_qpc_100ns - 200000 : 0;
    info.reference_usable = offset >= 2048;
    info.reset = offset == 2048;
    capture.append(mic.data(), reference.data(), output.data(), 2048, info);
    offset += 2048;
  }
  tracking = false;
  check(allocations == 0, "audio append allocated");
  check(capture.captured_frames() == 24000 && capture.total_frames() == 24000,
        "capture did not trim exact three-second endpoint");
  check(capture.write_complete() && capture.state() == LocalCaptureState::saved,
        "cannot finish local capture");
  std::locale::global(saved_locale);
  const auto path = capture.saved_directory();
  const auto mic_wave = read_text(path / "mic_input.wav");
  const auto ref_wave = read_text(path / "reference.wav");
  const auto out_wave = read_text(path / "processed.wav");
  check(mic_wave.size() == 44 + 24000 * 4 && le(mic_wave, 20, 2) == 3 &&
            le(mic_wave, 22, 2) == 1 && le(mic_wave, 24, 4) == 8000,
        "capture did not export exact mono IEEE float32 WAV");
  for (std::uint32_t i = 0; i < 24000; ++i) {
    check(std::bit_cast<float>(le(mic_wave, 44 + i * 4, 4)) == mic[i % 2048],
          "mic WAV does not contain exact host PCM");
    check(std::bit_cast<float>(le(out_wave, 44 + i * 4, 4)) == output[i % 2048],
          "output WAV does not contain exact returned PCM");
    const auto expected_ref = i < 2048 ? 0 : reference[i % 2048];
    check(std::bit_cast<float>(le(ref_wave, 44 + i * 4, 4)) == expected_ref,
          "reference WAV failed working-reference validity convention");
  }
  const auto json = read_text(path / "timeline.json");
  check(json.find("654321987") == std::string::npos &&
            json.find("/home/") == std::string::npos &&
            json.find("\\Users\\") == std::string::npos,
        "timeline exposed absolute timestamps or private paths");
  check(json.find("\"time_ms\":null") != std::string::npos &&
            json.find("\"reference_time_ms\":-20") != std::string::npos &&
            json.find("\"source_block_frames\":2048") != std::string::npos &&
            json.find("999999") == std::string::npos,
        "timeline lost invalid clock or trimmed-block semantics");
  const auto original = mic_wave;
  check(capture.arm(parent, 8000), "cannot arm subsequent capture");
  info.reference_usable = true;
  for (int i = 0; i < 12; ++i)
    capture.append(mic.data(), reference.data(), output.data(), 2048, info);
  check(capture.write_complete() && capture.saved_directory() != path &&
            read_text(path / "mic_input.wav") == original,
        "subsequent capture overwrote a previous session");
  if (const char *export_path = std::getenv("BUNCANCEL_LOCAL_CAPTURE_EXPORT")) {
    std::filesystem::create_directories(export_path);
    std::filesystem::copy(
        path, std::filesystem::path(export_path) / "synthetic-capture",
        std::filesystem::copy_options::recursive);
  }
}

void cancellation_contract(const std::filesystem::path &parent) {
  LocalCapture capture;
  std::array<float, 512> samples{};
  samples.fill(0.0125F);
  LocalCaptureBlock info;
  for (int repeat = 0; repeat < 20; ++repeat) {
    check(capture.arm(parent, 8000), "cannot arm cancellation trial");
    std::atomic<bool> started{};
    std::thread audio([&] {
      started.store(true, std::memory_order_release);
      for (int i = 0; i < 100; ++i)
        capture.append(samples.data(), samples.data(), samples.data(), 512,
                       info);
    });
    while (!started.load(std::memory_order_acquire))
      std::this_thread::yield();
    capture.cancel();
    audio.join();
    capture.cancel();
    check(!capture.write_complete() &&
              capture.state() == LocalCaptureState::cancelled,
          "cancellation saved private audio or lost ownership handoff");
    check(capture.saved_directory().empty(),
          "cancelled capture exposed output path");
  }
  check(std::filesystem::is_empty(parent),
        "cancelled session left audio on disk");
  check(!capture.arm(parent, 0) &&
            capture.error() == LocalCaptureError::invalid_rate,
        "invalid rate accepted");
  check(
      !capture.arm(std::filesystem::path("//invalid-network-location/capture"),
                   8000) &&
          capture.error() == LocalCaptureError::invalid_directory,
      "network destination accepted");
}

void failed_save_contract(const std::filesystem::path &parent) {
  LocalCapture capture;
  std::array<float, 2048> samples{};
  samples.fill(0.5F);
  LocalCaptureBlock info;
  check(capture.arm(parent, 8000), "cannot arm save-failure trial");
  for (int block = 0; block < 12; ++block)
    capture.append(samples.data(), nullptr, samples.data(), 2048, info);
  check(capture.state() == LocalCaptureState::complete,
        "save-failure trial did not complete");
  std::filesystem::remove(parent);
  check(!capture.write_complete() &&
            capture.state() == LocalCaptureState::failed &&
            capture.error() == LocalCaptureError::write_failed &&
            capture.saved_directory().empty(),
        "filesystem failure did not leave a clear failed session");
  check(!std::filesystem::exists(parent),
        "failed save unexpectedly recreated destination");
  check(capture.arm(parent, 8000), "failed session could not be replaced");
  for (int block = 0; block < 12; ++block)
    capture.append(samples.data(), nullptr, samples.data(), 2048, info);
  capture.cancel();
  check(!capture.write_complete() &&
            capture.state() == LocalCaptureState::cancelled &&
            std::filesystem::is_empty(parent),
        "cancellation after completion wrote unwanted audio");
}
} // namespace

int main() {
  std::filesystem::path directory;
  try {
    directory =
        std::filesystem::temp_directory_path() / "buncancel-local-capture-test";
    for (std::uint32_t i = 0; !std::filesystem::create_directory(directory);
         ++i)
      directory = std::filesystem::temp_directory_path() /
                  ("buncancel-local-capture-test-" + std::to_string(i));
    exact_session(directory / "complete");
    cancellation_contract(directory / "cancelled");
    failed_save_contract(directory / "failed");
    std::filesystem::remove_all(directory);
    std::cout << "Opt-in local capture tests passed, with zero audio-thread "
                 "allocations.\n";
    return 0;
  } catch (const std::exception &error) {
    if (!directory.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
    std::cerr << error.what() << '\n';
    return 1;
  }
}
