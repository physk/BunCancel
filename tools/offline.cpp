#include "aec.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Wave {
  int sampleRate;
  unsigned channels;
  std::vector<float> mono;
};

std::uint32_t le(const unsigned char *p, unsigned bytes) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < bytes; ++i) {
    value |= static_cast<std::uint32_t>(p[i]) << (8 * i);
  }
  return value;
}

Wave readWave(const std::string &path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("cannot open input WAV");
  }
  const auto length = stream.tellg();
  if (length < 12 || length > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("invalid WAV size");
  }
  std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
  stream.seekg(0);
  if (!stream.read(reinterpret_cast<char *>(bytes.data()), length)) {
    throw std::runtime_error("cannot read input WAV");
  }
  const auto tag = [&](std::size_t offset, const char *value) {
    return std::equal(bytes.begin() + offset, bytes.begin() + offset + 4,
                      value);
  };
  if (!tag(0, "RIFF") || !tag(8, "WAVE") ||
      le(bytes.data() + 4, 4) != bytes.size() - 8) {
    throw std::runtime_error("expected complete little-endian RIFF/WAVE");
  }
  unsigned format = 0, channels = 0, bits = 0, alignment = 0;
  std::uint32_t rate = 0;
  bool foundFormat = false, foundData = false;
  std::size_t dataOffset = 0, dataSize = 0;
  for (std::size_t pos = 12; pos < bytes.size();) {
    if (bytes.size() - pos < 8) {
      throw std::runtime_error("truncated WAV chunk header");
    }
    const std::size_t size = le(bytes.data() + pos + 4, 4);
    const std::size_t start = pos + 8;
    if (size > bytes.size() - start ||
        (size & 1U) > bytes.size() - start - size) {
      throw std::runtime_error("truncated WAV chunk");
    }
    if (tag(pos, "fmt ")) {
      if (foundFormat || size < 16) {
        throw std::runtime_error("invalid WAV format chunk");
      }
      foundFormat = true;
      format = le(bytes.data() + start, 2);
      channels = le(bytes.data() + start + 2, 2);
      rate = le(bytes.data() + start + 4, 4);
      alignment = le(bytes.data() + start + 12, 2);
      bits = le(bytes.data() + start + 14, 2);
      if ((channels != 1 && channels != 2) || rate == 0 || rate > 384000 ||
          !((format == 1 && (bits == 16 || bits == 24 || bits == 32)) ||
            (format == 3 && bits == 32)) ||
          alignment != channels * (bits / 8) ||
          le(bytes.data() + start + 8, 4) != rate * alignment) {
        throw std::runtime_error("unsupported or inconsistent WAV format");
      }
    } else if (tag(pos, "data")) {
      if (foundData) {
        throw std::runtime_error("duplicate WAV data chunk");
      }
      foundData = true;
      dataOffset = start;
      dataSize = size;
    }
    pos = start + size + (size & 1U);
  }
  if (!foundFormat || !foundData || dataSize == 0 ||
      dataSize % alignment != 0) {
    throw std::runtime_error("missing or incomplete WAV audio");
  }
  Wave result{static_cast<int>(rate), channels, {}};
  result.mono.reserve(dataSize / alignment);
  const unsigned width = bits / 8;
  for (std::size_t frame = 0; frame < dataSize; frame += alignment) {
    double sum = 0;
    for (unsigned channel = 0; channel < channels; ++channel) {
      const auto raw =
          le(bytes.data() + dataOffset + frame + channel * width, width);
      float value;
      if (format == 3) {
        value = std::bit_cast<float>(raw);
      } else {
        const auto signedValue =
            static_cast<std::int64_t>(raw) -
            ((raw & (1U << (bits - 1))) ? (1LL << bits) : 0);
        value = static_cast<float>(static_cast<double>(signedValue) /
                                   static_cast<double>(1ULL << (bits - 1)));
      }
      if (!std::isfinite(value)) {
        throw std::runtime_error("non-finite WAV sample");
      }
      sum += value;
    }
    // Both reference and microphone stereo inputs use an explicit equal-weight
    // mixdown.
    result.mono.push_back(static_cast<float>(sum / channels));
  }
  return result;
}

void put(std::ostream &stream, std::uint32_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    stream.put(static_cast<char>((value >> (i * 8)) & 255));
  }
}

void writeWave(const std::string &path, int rate,
               const std::vector<float> &samples) {
  if (samples.size() >
      (std::numeric_limits<std::uint32_t>::max() - 36ULL) / 2) {
    throw std::runtime_error("output exceeds RIFF size limit");
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot open output WAV");
  }
  const auto dataSize = static_cast<std::uint32_t>(samples.size() * 2);
  stream.write("RIFF", 4);
  put(stream, dataSize + 36, 4);
  stream.write("WAVEfmt ", 8);
  put(stream, 16, 4);
  put(stream, 1, 2);
  put(stream, 1, 2);
  put(stream, rate, 4);
  put(stream, rate * 2, 4);
  put(stream, 2, 2);
  put(stream, 16, 2);
  stream.write("data", 4);
  put(stream, dataSize, 4);
  for (float value : samples) {
    const auto quantized = static_cast<std::int32_t>(std::clamp(
        std::round(static_cast<double>(value) * 32768.0), -32768.0, 32767.0));
    put(stream, static_cast<std::uint32_t>(quantized), 2);
  }
  stream.flush();
  if (!stream) {
    throw std::runtime_error("cannot write output WAV");
  }
}

double rms(const std::vector<float> &samples) {
  double energy = 0;
  for (float value : samples) {
    energy += static_cast<double>(value) * value;
  }
  return std::sqrt(energy / samples.size());
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 4 && argc != 5) {
      throw std::runtime_error("usage: buncancel-offline reference.wav mic.wav "
                               "processed.wav [block-size]");
    }
    int block = 256;
    if (argc == 5) {
      std::size_t used = 0;
      block = std::stoi(argv[4], &used);
      if (used != std::string(argv[4]).size() || block < 1 || block > 65536) {
        throw std::runtime_error(
            "block-size must be an integer between 1 and 65536");
      }
    }
    const auto reference = readWave(argv[1]);
    const auto mic = readWave(argv[2]);
    if (reference.sampleRate != mic.sampleRate ||
        reference.mono.size() != mic.mono.size()) {
      throw std::runtime_error(
          "reference and microphone sample rates and frame counts must match");
    }
    buncancel::Aec engine(mic.sampleRate);
    std::vector<float> output(mic.mono.size());
    std::vector<double> times;
    times.reserve((output.size() + block - 1) / block);
    std::size_t deadlineMisses = 0;
    double totalUs = 0;
    for (std::size_t offset = 0; offset < output.size(); offset += block) {
      const int count = static_cast<int>(
          std::min<std::size_t>(block, output.size() - offset));
      const auto start = std::chrono::steady_clock::now();
      engine.process(mic.mono.data() + offset, reference.mono.data() + offset,
                     output.data() + offset, count);
      const double elapsed = std::chrono::duration<double, std::micro>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
      times.push_back(elapsed);
      totalUs += elapsed;
      deadlineMisses += elapsed > 1e6 * count / mic.sampleRate;
    }
    for (float value : output) {
      if (!std::isfinite(value)) {
        throw std::runtime_error("DSP produced non-finite output");
      }
    }
    const auto clippedSamples =
        std::count_if(output.begin(), output.end(), [](float value) {
          return value < -1.0F || value > 32767.0F / 32768.0F;
        });
    const double dspOutputRms = rms(output);
    for (float &value : output) {
      value = static_cast<float>(
          std::clamp(std::round(static_cast<double>(value) * 32768.0), -32768.0,
                     32767.0) /
          32768.0);
    }
    writeWave(argv[3], mic.sampleRate, output);
    std::sort(times.begin(), times.end());
    const auto percentile = [&](double fraction) {
      return times[static_cast<std::size_t>(
                       std::ceil(fraction * times.size())) -
                   1];
    };
    // No stems/window labels are supplied, so attenuation must never be
    // labelled ERLE.
    std::cout << std::setprecision(10) << "{\"sample_rate\":" << mic.sampleRate
              << ",\"frames\":" << output.size() << ",\"block_size\":" << block
              << ",\"reference_channels\":" << reference.channels
              << ",\"mic_channels\":" << mic.channels
              << ",\"stereo_mixdown\":\"arithmetic_mean\""
              << ",\"input_rms\":" << rms(mic.mono)
              << ",\"output_rms\":" << rms(output)
              << ",\"dsp_output_rms\":" << dspOutputRms
              << ",\"erle_db\":null,\"near_end_fidelity\":null"
              << ",\"output_format\":\"pcm16\",\"output_clipped_samples\":"
              << clippedSamples
              << ",\"latency_samples\":" << engine.latencySamples()
              << ",\"alignment_confidence\":" << engine.alignmentConfidence()
              << ",\"delay_samples\":" << engine.delaySamples()
              << ",\"aligned\":" << (engine.aligned() ? "true" : "false")
              << ",\"process_total_us\":" << totalUs
              << ",\"process_max_us\":" << times.back()
              << ",\"process_p50_us\":" << percentile(0.50)
              << ",\"process_p99_us\":" << percentile(0.99)
              << ",\"deadline_misses\":" << deadlineMisses << "}\n";
  } catch (const std::exception &error) {
    std::cerr << "buncancel-offline: " << error.what() << '\n';
    return 1;
  }
}
