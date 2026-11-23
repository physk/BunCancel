#include "aec.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

// These hooks observe the replaceable C++ allocation family, including aligned
// and nothrow forms. They do not interpose direct malloc calls or OS
// allocators. The real core currently owns its storage through std::vector.
namespace {
bool tracking = false;
std::size_t allocationCalls = 0;
std::size_t deallocationCalls = 0;

void *allocate(std::size_t size) {
  if (tracking) {
    ++allocationCalls;
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *allocateAligned(std::size_t size, std::size_t alignment) {
  if (tracking) {
    ++allocationCalls;
  }
  size = std::max<std::size_t>(size, 1);
#ifdef _WIN32
  void *memory = _aligned_malloc(size, alignment);
#else
  if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
    throw std::bad_alloc();
  }
  const auto roundedSize = (size + alignment - 1) / alignment * alignment;
  void *memory = std::aligned_alloc(alignment, roundedSize);
#endif
  if (memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc();
}

void release(void *memory) noexcept {
  if (tracking && memory != nullptr) {
    ++deallocationCalls;
  }
  std::free(memory);
}

void releaseAligned(void *memory) noexcept {
  if (tracking && memory != nullptr) {
    ++deallocationCalls;
  }
#ifdef _WIN32
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}
} // namespace

void *operator new(std::size_t size) { return allocate(size); }
void *operator new[](std::size_t size) { return allocate(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept {
  return ::operator new(size, tag);
}
void *operator new(std::size_t size, std::align_val_t alignment) {
  return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  try {
    return allocateAligned(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t &tag) noexcept {
  return ::operator new(size, alignment, tag);
}

void operator delete(void *memory) noexcept { release(memory); }
void operator delete[](void *memory) noexcept { release(memory); }
void operator delete(void *memory, std::size_t) noexcept { release(memory); }
void operator delete[](void *memory, std::size_t) noexcept { release(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept {
  release(memory);
}
void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  release(memory);
}
void operator delete(void *memory, std::align_val_t) noexcept {
  releaseAligned(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
  releaseAligned(memory);
}
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
  releaseAligned(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
  releaseAligned(memory);
}
void operator delete(void *memory, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  releaseAligned(memory);
}
void operator delete[](void *memory, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  releaseAligned(memory);
}

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void positiveControl() {
  allocationCalls = 0;
  deallocationCalls = 0;
  tracking = true;
  // Explicit calls avoid the language permission to elide new-expressions.
  void *scalar = ::operator new(17);
  void *array = ::operator new[](19);
  void *nothrowScalar = ::operator new(23, std::nothrow);
  void *nothrowArray = ::operator new[](29, std::nothrow);
  constexpr auto alignment = std::align_val_t{64};
  void *alignedScalar = ::operator new(31, alignment);
  void *alignedArray = ::operator new[](37, alignment);
  void *alignedNothrowScalar = ::operator new(41, alignment, std::nothrow);
  void *alignedNothrowArray = ::operator new[](43, alignment, std::nothrow);
  ::operator delete(scalar);
  ::operator delete[](array);
  ::operator delete(nothrowScalar, std::nothrow);
  ::operator delete[](nothrowArray, std::nothrow);
  ::operator delete(alignedScalar, alignment);
  ::operator delete[](alignedArray, alignment);
  ::operator delete(alignedNothrowScalar, alignment, std::nothrow);
  ::operator delete[](alignedNothrowArray, alignment, std::nothrow);
  tracking = false;
  require(allocationCalls == 8, "positive allocation control failed");
  require(deallocationCalls == 8, "positive deallocation control failed");
}

void coreStorageControl() {
  allocationCalls = 0;
  deallocationCalls = 0;
  tracking = true;
  {
    buncancel::Aec engine(48000);
    engine.reset();
  }
  tracking = false;
  require(allocationCalls > 0,
          "allocation hooks did not observe actual core construction");
  require(deallocationCalls == allocationCalls,
          "core storage allocation/deallocation controls disagree");
}

void exercise(int block) {
  constexpr int rate = 48000;
  constexpr int frames = rate * 2;
  constexpr int delay = 379;
  buncancel::Aec engine(rate);
  std::vector<float> reference(frames);
  std::vector<float> mic(frames);
  std::vector<float> output(frames);
  std::vector<float> silence(rate);
  std::uint32_t seed = 173;
  for (int i = 0; i < frames; ++i) {
    seed = seed * 1664525U + 1013904223U;
    reference[i] = (static_cast<float>(seed >> 8) / 16777216.0F - 0.5F) * 0.5F;
    if (i >= delay) {
      mic[i] = reference[i - delay] * 0.65F;
    }
  }
  const auto process = [&](const float *input, const float *ref, int count) {
    for (int offset = 0; offset < count; offset += block) {
      engine.process(input + offset, ref == nullptr ? nullptr : ref + offset,
                     output.data() + offset, std::min(block, count - offset));
    }
  };
  allocationCalls = 0;
  deallocationCalls = 0;
  tracking = true;
  process(mic.data(), reference.data(), frames);
  const bool initiallyAligned = engine.aligned();
  const auto active = engine.diagnostics();
  process(silence.data(), silence.data(), rate);
  const bool lostOnSilence = !engine.aligned();
  process(mic.data(), reference.data(), frames);
  const bool reacquiredAfterSilence = engine.aligned();
  process(mic.data(), nullptr, frames);
  const bool lostOnDropout = !engine.aligned();
  const auto disconnected = engine.diagnostics();
  const bool dropoutIsDry = output == mic;
  process(mic.data(), reference.data(), frames);
  const bool reacquiredAfterDropout = engine.aligned();
  engine.reset();
  const bool resetIsUnaligned = !engine.aligned();
  process(mic.data(), reference.data(), frames);
  const bool reacquiredAfterReset = engine.aligned();
  tracking = false;
  require(allocationCalls == 0, "prepared process/reset allocated memory");
  require(deallocationCalls == 0, "prepared process/reset freed memory");
  require(active.adaptedSamples > 0 && active.predictionRms > 0 &&
              disconnected.adaptedSamples == 0 &&
              !disconnected.subtractionActive,
          "allocation test did not observe active and disconnected telemetry");
  require(initiallyAligned,
          "allocation test did not exercise aligned processing");
  require(lostOnSilence && reacquiredAfterSilence,
          "allocation test did not exercise silence/reacquisition");
  require(lostOnDropout && dropoutIsDry && reacquiredAfterDropout,
          "allocation test did not exercise dry dropout/reacquisition");
  require(resetIsUnaligned && reacquiredAfterReset,
          "allocation test did not exercise reset/reacquisition");
  require(std::all_of(output.begin(), output.end(),
                      [](float value) { return std::isfinite(value); }),
          "allocation test produced non-finite output");
}
} // namespace

int main() {
  try {
    positiveControl();
    coreStorageControl();
    for (int block : {64, 128, 2048}) {
      exercise(block);
    }
    std::cout
        << "Allocation hooks positive control passed; process/reset used "
           "zero C++ allocations and deallocations at 64/128/2048 frames.\n";
  } catch (const std::exception &error) {
    tracking = false;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
