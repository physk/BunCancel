#include "local_capture.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace buncancel::plugin {
namespace {
constexpr std::size_t maximum_blocks = 16384;

bool local_directory(const std::filesystem::path &path) {
  const auto text = path.generic_string();
  if (text.starts_with("//") || text.starts_with("\\\\"))
    return false;
#ifdef _WIN32
  const auto root = std::filesystem::absolute(path).root_path().wstring();
  if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
    return false;
  auto part = std::filesystem::absolute(path);
  for (; !part.empty(); part = part.parent_path()) {
    const auto flags = GetFileAttributesW(part.c_str());
    if (flags != INVALID_FILE_ATTRIBUTES &&
        (flags & FILE_ATTRIBUTE_REPARSE_POINT))
      return false;
    if (part == part.root_path())
      break;
  }
#endif
  return true;
}

void little(std::ostream &stream, std::uint32_t value, int bytes) {
  for (int i = 0; i < bytes; ++i)
    stream.put(static_cast<char>((value >> (8 * i)) & 255));
}

void write_wave(const std::filesystem::path &path,
                const std::vector<float> &audio, std::uint32_t frames,
                std::uint32_t sample_rate) {
  std::ofstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot create local diagnostic WAV");
  file.write("RIFF", 4);
  little(file, 36 + frames * 4, 4);
  file.write("WAVEfmt ", 8);
  little(file, 16, 4);
  little(file, 3, 2); // IEEE float32, retaining the actual sample values.
  little(file, 1, 2);
  little(file, sample_rate, 4);
  little(file, sample_rate * 4, 4);
  little(file, 4, 2);
  little(file, 32, 2);
  file.write("data", 4);
  little(file, frames * 4, 4);
  for (std::uint32_t i = 0; i < frames; ++i)
    little(file, std::bit_cast<std::uint32_t>(audio[i]), 4);
  file.close();
  if (!file)
    throw std::runtime_error("cannot complete local diagnostic WAV");
}

void number(std::ostream &stream, double value) {
  if (std::isfinite(value))
    stream << value;
  else
    stream << "null";
}

double relative_ms(std::uint64_t value, std::uint64_t origin) noexcept {
  return (static_cast<double>(value) - static_cast<double>(origin)) / 10000.0;
}
} // namespace

bool LocalCapture::arm(const std::filesystem::path &parent,
                       std::uint32_t sample_rate) {
  auto current = state_.load(std::memory_order_acquire);
  if (current != LocalCaptureState::idle &&
      current != LocalCaptureState::saved &&
      current != LocalCaptureState::cancelled &&
      current != LocalCaptureState::failed)
    return false;
  if (!state_.compare_exchange_strong(current, LocalCaptureState::preparing,
                                      std::memory_order_acq_rel))
    return false;
  cancellation_requested_.store(false, std::memory_order_release);
  error_.store(LocalCaptureError::none, std::memory_order_relaxed);
  frames_.store(0, std::memory_order_relaxed);
  target_frames_.store(0, std::memory_order_relaxed);
  entries_ = 0;
  saved_directory_.clear();
  erase_audio();
  if (sample_rate < 8000 || sample_rate > 192000) {
    error_.store(LocalCaptureError::invalid_rate, std::memory_order_relaxed);
    state_.store(LocalCaptureState::failed, std::memory_order_release);
    return false;
  }
  try {
    if (parent.empty() || !local_directory(parent)) {
      error_.store(LocalCaptureError::invalid_directory,
                   std::memory_order_relaxed);
      state_.store(LocalCaptureState::failed, std::memory_order_release);
      return false;
    }
    std::filesystem::create_directories(parent);
    parent_ = std::filesystem::canonical(parent);
    if (!std::filesystem::is_directory(parent_) || !local_directory(parent_))
      throw std::runtime_error(
          "diagnostic destination is not a local directory");
  } catch (...) {
    error_.store(LocalCaptureError::invalid_directory,
                 std::memory_order_relaxed);
    state_.store(LocalCaptureState::failed, std::memory_order_release);
    return false;
  }
  sample_rate_ = sample_rate;
  const auto target = sample_rate * 3;
  try {
    microphone_.assign(target, 0);
    reference_.assign(target, 0);
    output_.assign(target, 0);
    timeline_.resize(maximum_blocks);
  } catch (...) {
    erase_audio();
    error_.store(LocalCaptureError::memory_unavailable,
                 std::memory_order_relaxed);
    state_.store(LocalCaptureState::failed, std::memory_order_release);
    return false;
  }
  target_frames_.store(target, std::memory_order_relaxed);
  state_.store(cancellation_requested_.load(std::memory_order_acquire)
                   ? LocalCaptureState::cancelled
                   : LocalCaptureState::recording,
               std::memory_order_release);
  return state() == LocalCaptureState::recording;
}

void LocalCapture::append(const float *microphone, const float *reference,
                          const float *output, std::uint32_t frames,
                          const LocalCaptureBlock &block) noexcept {
  auto expected = LocalCaptureState::recording;
  if (!state_.compare_exchange_strong(expected, LocalCaptureState::appending,
                                      std::memory_order_acq_rel))
    return;
  if (cancellation_requested_.load(std::memory_order_acquire)) {
    state_.store(LocalCaptureState::cancelled, std::memory_order_release);
    return;
  }
  if (!microphone || !output || frames == 0) {
    error_.store(LocalCaptureError::invalid_audio, std::memory_order_relaxed);
    state_.store(LocalCaptureState::failed, std::memory_order_release);
    return;
  }
  if (entries_ == timeline_.size()) {
    error_.store(LocalCaptureError::metadata_overflow,
                 std::memory_order_relaxed);
    state_.store(LocalCaptureState::failed, std::memory_order_release);
    return;
  }
  const auto offset = frames_.load(std::memory_order_relaxed);
  const auto target = target_frames_.load(std::memory_order_relaxed);
  const auto count = std::min(frames, target - offset);
  std::copy_n(microphone, count, microphone_.data() + offset);
  std::copy_n(output, count, output_.data() + offset);
  if (reference && block.reference_usable)
    std::copy_n(reference, count, reference_.data() + offset);
  else
    std::fill_n(reference_.data() + offset, count, 0);
  auto &entry = timeline_[entries_++];
  entry = {offset, count, frames, block};
  entry.block.diagnostics.changed_samples = 0;
  double mic_energy = 0, output_energy = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    mic_energy += static_cast<double>(microphone[i]) * microphone[i];
    output_energy += static_cast<double>(output[i]) * output[i];
    entry.block.diagnostics.changed_samples += microphone[i] != output[i];
  }
  entry.block.diagnostics.mic_rms =
      static_cast<float>(std::sqrt(mic_energy / count));
  entry.block.diagnostics.output_rms =
      static_cast<float>(std::sqrt(output_energy / count));
  entry.block.reference_usable = reference && block.reference_usable;
  frames_.store(offset + count, std::memory_order_release);
  state_.store(cancellation_requested_.load(std::memory_order_acquire)
                   ? LocalCaptureState::cancelled
               : offset + count == target ? LocalCaptureState::complete
                                          : LocalCaptureState::recording,
               std::memory_order_release);
  // A concurrent cancel can observe Appending after the first flag load. The
  // second check closes that handoff even when the host never calls us again.
  if (cancellation_requested_.load(std::memory_order_acquire))
    cancel();
}

void LocalCapture::cancel() noexcept {
  cancellation_requested_.store(true, std::memory_order_release);
  auto expected = LocalCaptureState::recording;
  if (state_.compare_exchange_strong(expected, LocalCaptureState::cancelled,
                                     std::memory_order_acq_rel))
    return;
  expected = LocalCaptureState::complete;
  state_.compare_exchange_strong(expected, LocalCaptureState::cancelled,
                                 std::memory_order_acq_rel);
}

void LocalCapture::erase_audio() noexcept {
  std::fill(microphone_.begin(), microphone_.end(), 0);
  std::fill(reference_.begin(), reference_.end(), 0);
  std::fill(output_.begin(), output_.end(), 0);
  std::fill(timeline_.begin(), timeline_.end(), Entry{});
  microphone_.clear();
  reference_.clear();
  output_.clear();
  timeline_.clear();
}

bool LocalCapture::write_complete() noexcept {
  auto expected = LocalCaptureState::complete;
  if (!state_.compare_exchange_strong(expected, LocalCaptureState::writing,
                                      std::memory_order_acq_rel)) {
    if (expected == LocalCaptureState::cancelled ||
        expected == LocalCaptureState::failed)
      erase_audio();
    return false;
  }
  std::filesystem::path destination;
  try {
    if (cancellation_requested_.load(std::memory_order_acquire))
      throw std::runtime_error("diagnostic capture cancelled before save");
    if (!local_directory(parent_))
      throw std::runtime_error("local diagnostic destination changed");
    for (std::uint32_t suffix = 1; suffix < 100000; ++suffix) {
      auto candidate = parent_ / ("capture-" + std::to_string(suffix));
      if (std::filesystem::create_directory(candidate)) {
        destination = std::move(candidate);
        break;
      }
    }
    if (destination.empty())
      throw std::runtime_error("cannot reserve a local diagnostic directory");
    const auto count = frames_.load(std::memory_order_acquire);
    write_wave(destination / "mic_input.wav", microphone_, count, sample_rate_);
    write_wave(destination / "reference.wav", reference_, count, sample_rate_);
    write_wave(destination / "processed.wav", output_, count, sample_rate_);
    std::ofstream json(destination / "timeline.json");
    if (!json)
      throw std::runtime_error("cannot create local diagnostic timeline");
    json.imbue(std::locale::classic());
    json << std::setprecision(10) << std::boolalpha;
    json << "{\"schema_version\":1,\"sample_rate\":" << sample_rate_
         << ",\"duration_seconds\":3,\"target_frames\":" << total_frames()
         << ",\"captured_frames\":" << count
         << ",\"complete\":true,\"reference_timebase\":\"microphone_samples\","
            "\"blocks\":[";
    std::uint64_t origin = 0;
    for (std::uint32_t i = 0; i < entries_ && !origin; ++i)
      origin = timeline_[i].block.microphone_qpc_100ns;
    for (std::uint32_t i = 0; i < entries_; ++i) {
      const auto &entry = timeline_[i];
      const auto &block = entry.block;
      const auto &info = block.diagnostics;
      if (i)
        json << ',';
      json << "{\"frame_offset\":" << entry.frame_offset
           << ",\"frames\":" << entry.frames
           << ",\"source_block_frames\":" << entry.source_block_frames
           << ",\"time_ms\":";
      if (origin && block.microphone_qpc_100ns)
        number(json, relative_ms(block.microphone_qpc_100ns, origin));
      else
        json << "null";
      json << ",\"reference_time_ms\":";
      if (block.reference_qpc_100ns && origin)
        number(json, relative_ms(block.reference_qpc_100ns, origin));
      else
        json << "null";
      json << ",\"reference_usable\":" << block.reference_usable
           << ",\"reset\":" << block.reset
           << ",\"status\":" << static_cast<std::uint32_t>(info.status)
           << ",\"aligned\":" << info.aligned
           << ",\"subtraction_active\":" << info.subtraction_active
           << ",\"reducing\":" << info.reducing << ",\"live_confidence\":";
      number(json, info.live_confidence);
      json << ",\"peak_correlation\":";
      number(json, info.correlation);
      json << ",\"active_delay_ms\":";
      number(json, info.active_delay_ms);
      json << ",\"adapted_samples\":" << info.adapted_samples
           << ",\"shadow_samples\":" << info.shadow_adapted_samples
           << ",\"promotions\":" << info.shadow_promotions
           << ",\"filter_resets\":" << info.filter_resets
           << ",\"underruns\":" << info.underruns
           << ",\"dropouts\":" << info.dropouts << ",\"mic_rms\":";
      number(json, info.mic_rms);
      json << ",\"raw_reference_rms\":";
      number(json, info.reference_rms);
      double reference_energy = 0;
      for (std::uint32_t sample = 0; sample < entry.frames; ++sample) {
        const float value = reference_[entry.frame_offset + sample];
        reference_energy += static_cast<double>(value) * value;
      }
      json << ",\"reference_rms\":";
      number(json, std::sqrt(reference_energy / entry.frames));
      json << ",\"output_rms\":";
      number(json, info.output_rms);
      json << ",\"changed_samples\":" << info.changed_samples
           << ",\"reference_receipt_age_ms\":";
      if (info.reference_receipt_valid)
        number(json, info.reference_receipt_age_ms);
      else
        json << "null";
      json << ",\"window_reference_rms\":";
      number(json, info.window_reference_rms);
      json << ",\"window_downmix_rms\":";
      number(json, info.window_downmix_rms);
      json << ",\"window_reference_seconds\":";
      number(json, info.window_reference_seconds);
      json << ",\"shadow_decision\":" << info.shadow_decision
           << ",\"shadow_validation_count\":" << info.shadow_validation_count
           << ",\"shadow_training_eligibility\":";
      number(json, info.shadow_training_eligibility);
      json << ",\"shadow_validation_eligibility\":";
      number(json, info.shadow_validation_eligibility);
      json << ",\"shadow_coherence\":";
      number(json, info.shadow_coherence);
      json << ",\"shadow_mic_error_ratio\":";
      number(json, info.shadow_mic_error_ratio);
      json << ",\"shadow_foreground_error_ratio\":";
      number(json, info.shadow_foreground_error_ratio);
      json << '}';
    }
    json << "]}\n";
    json.close();
    if (!json)
      throw std::runtime_error("cannot complete local diagnostic timeline");
    if (cancellation_requested_.load(std::memory_order_acquire))
      throw std::runtime_error("diagnostic capture cancelled during save");
    saved_directory_ = destination;
    erase_audio();
    state_.store(LocalCaptureState::saved, std::memory_order_release);
    return true;
  } catch (...) {
    if (!destination.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(destination, ignored);
    }
    erase_audio();
    const bool cancelled =
        cancellation_requested_.load(std::memory_order_acquire);
    error_.store(cancelled ? LocalCaptureError::none
                           : LocalCaptureError::write_failed,
                 std::memory_order_relaxed);
    state_.store(cancelled ? LocalCaptureState::cancelled
                           : LocalCaptureState::failed,
                 std::memory_order_release);
    return false;
  }
}

LocalCaptureState LocalCapture::state() const noexcept {
  return state_.load(std::memory_order_acquire);
}
LocalCaptureError LocalCapture::error() const noexcept {
  return error_.load(std::memory_order_acquire);
}
std::uint32_t LocalCapture::captured_frames() const noexcept {
  return frames_.load(std::memory_order_acquire);
}
std::uint32_t LocalCapture::total_frames() const noexcept {
  return target_frames_.load(std::memory_order_acquire);
}
const std::filesystem::path &LocalCapture::saved_directory() const noexcept {
  return saved_directory_;
}

} // namespace buncancel::plugin
