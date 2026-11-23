#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace buncancel::capture::detail {

// One producer, one consumer, fixed lifetime. Never reset indices concurrently.
// Full queues drop the incoming packet; producer never touches consumer state.
template <typename Packet, std::size_t Capacity> class PacketQueue {
  static_assert(Capacity > 1);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
  bool push(const Packet &packet) noexcept {
    const auto write = write_.load(std::memory_order_relaxed);
    if (write - read_.load(std::memory_order_acquire) == Capacity) {
      return false;
    }
    packets_[write % Capacity] = packet;
    write_.store(write + 1, std::memory_order_release);
    return true;
  }

  bool pop(Packet &packet) noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) {
      return false;
    }
    packet = packets_[read % Capacity];
    read_.store(read + 1, std::memory_order_release);
    return true;
  }

private:
  std::array<Packet, Capacity> packets_{};
  alignas(64) std::atomic<std::uint64_t> write_{};
  alignas(64) std::atomic<std::uint64_t> read_{};
};

} // namespace buncancel::capture::detail
