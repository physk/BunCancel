#pragma once

#include "diagnostics.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace buncancel::plugin {

enum class LocalCaptureError : std::uint32_t {
  none,
  invalid_rate,
  invalid_directory,
  memory_unavailable,
  invalid_audio,
  metadata_overflow,
  write_failed
};

struct LocalCaptureBlock {
  std::uint64_t microphone_qpc_100ns{};
  std::uint64_t reference_qpc_100ns{};
  Diagnostics diagnostics{};
  bool reference_usable{};
  bool reset{};
};

// Explicit opt-in only. arm() and write_complete() belong to one serialized
// control thread. append() belongs to one audio thread. cancel() may be called
// concurrently, and never waits. Destruction requires a quiescent audio thread.
// No samples are retained before a successful arm(). Each session lasts 3 s.
class LocalCapture {
public:
  bool arm(const std::filesystem::path &parent, std::uint32_t sample_rate);
  void append(const float *microphone, const float *reference,
              const float *output, std::uint32_t frames,
              const LocalCaptureBlock &block) noexcept;
  void cancel() noexcept;
  // Saves only a completed capture, or erases a cancelled/failed session's
  // buffers. Returns true only after all four files have been written.
  bool write_complete() noexcept;
  LocalCaptureState state() const noexcept;
  LocalCaptureError error() const noexcept;
  std::uint32_t captured_frames() const noexcept;
  std::uint32_t total_frames() const noexcept;
  // Control-thread only. The path is empty until a successful save.
  const std::filesystem::path &saved_directory() const noexcept;

private:
  static_assert(std::atomic<LocalCaptureState>::is_always_lock_free);
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
  static_assert(std::atomic<bool>::is_always_lock_free);
  struct Entry {
    std::uint32_t frame_offset{};
    std::uint32_t frames{};
    std::uint32_t source_block_frames{};
    LocalCaptureBlock block{};
  };
  void erase_audio() noexcept;
  std::atomic<LocalCaptureState> state_{LocalCaptureState::idle};
  std::atomic<LocalCaptureError> error_{LocalCaptureError::none};
  std::atomic<bool> cancellation_requested_{};
  std::atomic<std::uint32_t> frames_{};
  std::atomic<std::uint32_t> target_frames_{};
  std::uint32_t sample_rate_{};
  std::uint32_t entries_{};
  std::filesystem::path parent_, saved_directory_;
  std::vector<float> microphone_, reference_, output_;
  std::vector<Entry> timeline_;
};

} // namespace buncancel::plugin
