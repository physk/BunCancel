// This executable compiles the production processor with a generated capture
// backend and deterministic clock. The distributed module has neither override.
// It verifies VST3 bus/state/callback wiring without opening a Windows
// endpoint.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shlobj.h>
#include <windows.h>

#include "reference_capture.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace generated_input {
constexpr int rate = 48000;
constexpr std::uint64_t epoch = 216000000000ULL;
std::atomic<int> callbackEnd{};
std::atomic<std::uint64_t> deliveredPackets{};
const std::vector<float> *left{};
const std::vector<float> *right{};
std::filesystem::path captureRoot;

std::uint64_t stamp(int frame) {
  return epoch + static_cast<std::uint64_t>(frame) * 10000000ULL / rate;
}
BOOL WINAPI counter(LARGE_INTEGER *value) {
  value->QuadPart = static_cast<LONGLONG>(stamp(callbackEnd.load()));
  return TRUE;
}
HRESULT WINAPI localFolder(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *value) {
  const auto path = captureRoot.wstring();
  const auto bytes = (path.size() + 1) * sizeof(wchar_t);
  *value = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
  if (!*value)
    return E_OUTOFMEMORY;
  std::memcpy(*value, path.c_str(), bytes);
  return S_OK;
}
BOOL WINAPI frequency(LARGE_INTEGER *value) {
  value->QuadPart = 10000000;
  return TRUE;
}
} // namespace generated_input

// Only this test translation unit substitutes the clock calls. The Windows
// declarations have already been included above.
#define QueryPerformanceCounter generated_input::counter
#define QueryPerformanceFrequency generated_input::frequency
#define SHGetKnownFolderPath generated_input::localFolder
#include "../../src/plugin/plugin.cpp"
#undef QueryPerformanceCounter
#undef QueryPerformanceFrequency
#undef SHGetKnownFolderPath

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

namespace buncancel::plugin {
Steinberg::IPlugView *createEditor(Steinberg::Vst::EditController *) {
  return nullptr;
}
} // namespace buncancel::plugin

namespace buncancel::capture {
struct ReferenceCapture::Impl {
  std::atomic<CaptureStatus> state{CaptureStatus::stopped};
  int nextFrame{};
};
ReferenceCapture::ReferenceCapture() : impl_(std::make_unique<Impl>()) {}
ReferenceCapture::~ReferenceCapture() = default;
std::vector<Endpoint> ReferenceCapture::enumerate_endpoints() {
  return {{"generated-test-reference", "Generated stereo reference",
           EndpointKind::capture}};
}
bool ReferenceCapture::start(const CaptureConfig &config) {
  if (config.endpoint_id != "generated-test-reference" ||
      config.sample_rate != generated_input::rate)
    return false;
  impl_->nextFrame = 0;
  impl_->state.store(CaptureStatus::running, std::memory_order_release);
  return true;
}
void ReferenceCapture::stop() noexcept {
  impl_->state.store(CaptureStatus::stopped, std::memory_order_release);
}
bool ReferenceCapture::pop(ReferencePacket &packet) noexcept {
  if (status() != CaptureStatus::running || !generated_input::left ||
      !generated_input::right)
    return false;
  constexpr int frames = 480;
  const int start = impl_->nextFrame;
  if (start + frames + 48 > generated_input::callbackEnd.load() ||
      start + frames > static_cast<int>(generated_input::left->size()))
    return false;
  packet = {};
  packet.frame_count = frames;
  packet.channel_count = 2;
  packet.sample_rate = generated_input::rate;
  packet.device_position = 9000000 + start;
  packet.qpc_position_100ns = generated_input::stamp(start);
  packet.generation = 7;
  packet.discontinuity = start == 0;
  for (int i = 0; i < frames; ++i) {
    packet.samples[2 * i] = (*generated_input::left)[start + i];
    packet.samples[2 * i + 1] = (*generated_input::right)[start + i];
  }
  impl_->nextFrame += frames;
  generated_input::deliveredPackets.fetch_add(1);
  return true;
}
CaptureStatus ReferenceCapture::status() const noexcept {
  return impl_->state.load(std::memory_order_acquire);
}
std::uint64_t ReferenceCapture::dropped_packets() const noexcept { return 0; }
std::uint64_t ReferenceCapture::discontinuities() const noexcept { return 0; }
std::uint32_t ReferenceCapture::actual_sample_rate() const noexcept {
  return generated_input::rate;
}
} // namespace buncancel::capture

namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;
constexpr int rate = generated_input::rate;
constexpr int frames = 12 * rate;
constexpr int delay = 13056;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<float> program(std::uint32_t seed) {
  std::vector<float> signal(frames);
  double smooth = 0;
  for (int i = 0; i < frames; ++i) {
    seed = seed * 1664525U + 1013904223U;
    const double noise = static_cast<double>(seed >> 8) / 16777216.0 - 0.5;
    smooth = 0.75 * smooth + 0.25 * noise;
    const double t = static_cast<double>(i) / rate;
    const double phase = 6.283185307179586 * (147 * t + 1.8 * std::sin(t));
    const double envelope = 0.3 + 0.7 * std::pow(std::sin(t * 8.3), 2);
    signal[i] =
        static_cast<float>(0.09 * envelope * (smooth + 0.12 * std::sin(phase)));
  }
  return signal;
}

void settings(MemoryStream &stream, bool bypass = false) {
  IBStreamer writer(&stream, kLittleEndian);
  const std::string endpoint = "generated-test-reference";
  require(writer.writeInt32(1) && writer.writeInt32(1) &&
              writer.writeInt32(bypass) &&
              writer.writeInt32(static_cast<int32>(endpoint.size())) &&
              writer.writeRaw(endpoint.data(),
                              static_cast<int32>(endpoint.size())) ==
                  static_cast<int32>(endpoint.size()),
          "cannot build generated endpoint settings");
  stream.seek(0, IBStream::kIBSeekSet, nullptr);
}

class ObservationPeer final : public Steinberg::Vst::IConnectionPoint {
public:
  buncancel::plugin::Diagnostics latest;
  tresult PLUGIN_API queryInterface(const TUID iid, void **object) override {
    *object = nullptr;
    if (FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IConnectionPoint::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
      *object = static_cast<Steinberg::Vst::IConnectionPoint *>(this);
      addRef();
      return kResultOk;
    }
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return ++references_; }
  uint32 PLUGIN_API release() override { return --references_; }
  tresult PLUGIN_API connect(Steinberg::Vst::IConnectionPoint *) override {
    return kResultOk;
  }
  tresult PLUGIN_API disconnect(Steinberg::Vst::IConnectionPoint *) override {
    return kResultOk;
  }
  tresult PLUGIN_API notify(IMessage *message) override {
    if (!message || !message->getAttributes())
      return kResultFalse;
    const void *bytes = nullptr;
    uint32 size = 0;
    if (message->getAttributes()->getBinary(
            buncancel::plugin::diagnosticsAttribute, bytes, size) !=
            kResultOk ||
        size != sizeof(latest))
      return kResultFalse;
    std::memcpy(&latest, bytes, size);
    return kResultOk;
  }

private:
  uint32 references_ = 1;
};

class Session {
public:
  Session() : processor_(new buncancel::plugin::Processor) {
    require(processor_->initialize(&host_) == kResultOk,
            "production component initialize failed");
    require(processor_->connect(&observer_) == kResultOk,
            "production processor diagnostic connection failed");
    SpeakerArrangement mono = SpeakerArr::kMono;
    require(processor_->setBusArrangements(&mono, 1, &mono, 1) == kResultOk,
            "production processor rejected mono input/output");
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    require(processor_->setBusArrangements(&stereo, 1, &stereo, 1) != kResultOk,
            "test did not verify the mono bus contract");
    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = rate;
    require(processor_->setupProcessing(setup) == kResultOk,
            "production setupProcessing failed");
    MemoryStream persisted;
    settings(persisted);
    require(processor_->setState(&persisted) == kResultOk,
            "production processor rejected selected generated reference");
    require(processor_->activateBus(kAudio, kInput, 0, true) == kResultOk &&
                processor_->activateBus(kAudio, kOutput, 0, true) ==
                    kResultOk &&
                processor_->setActive(true) == kResultOk &&
                processor_->setProcessing(true) == kResultOk,
            "production host lifecycle activation failed");
    // The production reference control thread is asynchronous. Allow its first
    // request to finish before advancing the generated sample clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  ~Session() {
    processor_->setProcessing(false);
    processor_->setActive(false);
    processor_->disconnect(&observer_);
    processor_->terminate();
    processor_->release();
  }
  void process(int start, float *mic, float *out, int count, bool silent,
               bool bypass = false) {
    float *inputChannel = mic;
    float *outputChannel = out;
    AudioBusBuffers input{}, output{};
    input.numChannels = output.numChannels = 1;
    input.silenceFlags = silent ? 1 : 0;
    input.channelBuffers32 = &inputChannel;
    output.channelBuffers32 = &outputChannel;
    ProcessContext context{};
    context.sampleRate = rate;
    context.state = ProcessContext::kContTimeValid;
    context.continousTimeSamples = start;
    ParameterChanges changes(1), statuses(1);
    int32 index = 0;
    changes.addParameterData(buncancel::plugin::bypassId, index)
        ->addPoint(0, bypass ? 1.0 : 0.0, index);
    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = count;
    data.numInputs = data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    data.processContext = &context;
    data.inputParameterChanges = &changes;
    data.outputParameterChanges = &statuses;
    generated_input::callbackEnd.store(start + count);
    IAudioProcessor *audioInterface = processor_;
    require(audioInterface->process(data) == kResultOk,
            "production IAudioProcessor::process failed");
    if (statuses.getParameterCount()) {
      auto *queue = statuses.getParameterData(0);
      int32 offset = 0;
      ParamValue normalized = 0;
      require(queue->getPoint(queue->getPointCount() - 1, offset, normalized) ==
                  kResultOk,
              "production status parameter output failed");
      lastStatus = static_cast<buncancel::plugin::ProcessingStatus>(std::lround(
          normalized *
          (static_cast<int>(buncancel::plugin::ProcessingStatus::count) - 1)));
    }
  }
  void capture(bool start) {
    HostMessage message;
    message.setMessageID(start ? buncancel::plugin::captureStart
                               : buncancel::plugin::captureCancel);
    require(processor_->notify(&message) == kResultOk,
            "production opt-in capture command failed");
    const auto expected = start
                              ? buncancel::plugin::LocalCaptureState::recording
                              : buncancel::plugin::LocalCaptureState::cancelled;
    for (int attempt = 0; attempt < 100; ++attempt) {
      HostMessage request;
      request.setMessageID(buncancel::plugin::statusRequest);
      require(processor_->notify(&request) == kResultOk,
              "production capture status request failed");
      if (observer_.latest.capture_state == expected)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(false, "production capture command did not reach expected state");
  }
  void awaitSaved() {
    for (int attempt = 0; attempt < 100; ++attempt) {
      HostMessage request;
      request.setMessageID(buncancel::plugin::statusRequest);
      require(processor_->notify(&request) == kResultOk,
              "production capture completion request failed");
      if (observer_.latest.capture_state ==
          buncancel::plugin::LocalCaptureState::saved)
        return;
      require(observer_.latest.capture_state !=
                  buncancel::plugin::LocalCaptureState::failed,
              "production opt-in export failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(false, "production opt-in export did not finish");
  }
  buncancel::plugin::ProcessingStatus lastStatus{
      buncancel::plugin::ProcessingStatus::unavailable};

private:
  HostApplication host_;
  ObservationPeer observer_;
  buncancel::plugin::Processor *processor_;
};

struct Window {
  double micEnergy{}, outEnergy{}, nearEnergy{}, nearCross{}, errorEnergy{};
  std::uint64_t changed{};
  void add(float mic, float out, float voice) {
    micEnergy += static_cast<double>(mic) * mic;
    outEnergy += static_cast<double>(out) * out;
    nearEnergy += static_cast<double>(voice) * voice;
    nearCross += static_cast<double>(out) * voice;
    const double error = out - voice;
    errorEnergy += error * error;
    changed += mic != out;
  }
};

void run(bool echo, bool quiet) {
  auto left = program(197);
  auto distinct = program(373);
  auto voice = program(991);
  std::vector<float> right(frames), mic(frames), output(frames);
  for (int i = 0; i < frames; ++i) {
    right[i] = 0.75F * left[i] + 0.25F * distinct[i];
    if (quiet) {
      left[i] *= 0.001F;
      right[i] *= 0.001F;
      voice[i] *= 0.001F;
    }
    const auto reference = [&](int index) {
      return index < 0 ? 0.0F : 0.5F * (left[index] + right[index]);
    };
    const bool playbackEcho = echo && i < 10 * rate;
    const float desired = !echo || i >= 7 * rate ? voice[i] : 0.0F;
    mic[i] = desired;
    if (playbackEcho)
      mic[i] += 1.3F * reference(i - delay) + 0.17F * reference(i - delay - 23);
  }
  generated_input::left = &left;
  generated_input::right = &right;
  generated_input::callbackEnd.store(0);
  generated_input::deliveredPackets.store(0);
  Session session;
  Window farWindow, overlap, independent;
  std::uint64_t readyBlocks = 0, cancellingBlocks = 0;
  for (int start = 0; start < frames; start += 512) {
    const int count = std::min(512, frames - start);
    if (echo && start == 4 * rate) {
      require(
          !std::filesystem::exists(generated_input::captureRoot / "BunCancel"),
          "production recorder created files before explicit opt-in");
      session.capture(true);
    }
    session.process(start, mic.data() + start, output.data() + start, count,
                    false);
    readyBlocks += session.lastStatus ==
                   buncancel::plugin::ProcessingStatus::readyMonitoring;
    cancellingBlocks +=
        session.lastStatus == buncancel::plugin::ProcessingStatus::cancelling;
    for (int i = 0; i < count; ++i) {
      const int time = start + i;
      require(std::isfinite(output[time]), "host emitted nonfinite output");
      if (!echo)
        require(
            output[time] == mic[time],
            "uncorrelated mono voice speech changed in production callback");
      const float desired = !echo || time >= 7 * rate ? voice[time] : 0.0F;
      if (time >= 4 * rate && time < 6 * rate)
        farWindow.add(mic[time], output[time], desired);
      if (time >= 8 * rate && time < 9 * rate)
        overlap.add(mic[time], output[time], desired);
      if (time >= 11 * rate)
        independent.add(mic[time], output[time], desired);
    }
  }
  require(generated_input::deliveredPackets.load() > 100,
          "generated stereo PCM never entered the production capture consumer");
  if (echo) {
    const double erle =
        10 * std::log10(farWindow.micEnergy / farWindow.outEnergy);
    const double nearGain = overlap.nearCross / overlap.nearEnergy;
    const double nearError = overlap.errorEnergy / overlap.nearEnergy;
    require(erle > 8 && farWindow.changed > rate,
            "production VST callback failed actual echo-only attenuation");
    require(
        nearGain > 0.90 && nearGain < 1.10 && nearError < 0.12,
        "production VST callback damaged simultaneous mono near-end speech");
    require(independent.errorEnergy == 0,
            "production VST callback retained stale subtraction after echo");
    require(cancellingBlocks > 0,
            "production status never reported measured cancellation");
    std::cout << "host_signal echo_only_erle_db=" << erle
              << " changed_samples=" << farWindow.changed
              << " double_talk_near_projection=" << nearGain
              << " double_talk_error_to_near=" << nearError
              << " near_only_tail_exact=true cancelling_blocks="
              << cancellingBlocks << '\n';
  } else {
    require(cancellingBlocks == 0 && readyBlocks > 100,
            "independent mono speech did not stay Ready in production host");
    std::cout << "host_signal independent_mono_exact=true quiet=" << quiet
              << " ready_blocks=" << readyBlocks << '\n';
  }
  if (echo) {
    const auto destination = generated_input::captureRoot / "BunCancel" /
                             "Diagnostics" / "capture-1";
    session.awaitSaved();
    for (const auto &name :
         {"mic_input.wav", "reference.wav", "processed.wav"}) {
      const auto path = destination / name;
      require(std::filesystem::file_size(path) == 44 + 3 * rate * sizeof(float),
              "production opt-in WAV is incomplete or has the wrong format");
      if (std::string(name) != "reference.wav") {
        std::ifstream file(path, std::ios::binary);
        file.seekg(44);
        std::vector<float> audio(3 * rate);
        file.read(reinterpret_cast<char *>(audio.data()),
                  static_cast<std::streamsize>(audio.size() * sizeof(float)));
        require(static_cast<bool>(file), "production opt-in WAV read failed");
        const auto &expected =
            std::string(name) == "mic_input.wav" ? mic : output;
        require(
            std::equal(audio.begin(), audio.end(), expected.begin() + 4 * rate),
            "saved opt-in WAV differs from actual production callback PCM");
      }
    }
    std::ifstream timeline(destination / "timeline.json");
    const std::string json((std::istreambuf_iterator<char>(timeline)), {});
    require(json.find("\"captured_frames\":144000") != std::string::npos &&
                json.find("\"complete\":true") != std::string::npos &&
                json.find("generated-test-reference") == std::string::npos &&
                json.find(generated_input::captureRoot.string()) ==
                    std::string::npos,
            "production timeline is incomplete or includes private path/device "
            "metadata");
    session.capture(true);
    session.capture(false);
    require(!std::filesystem::exists(destination.parent_path() / "capture-2"),
            "cancelled production capture saved unexpected audio");
    std::cout
        << "host_capture explicit_opt_in=true saved_callback_pcm_exact=true "
           "cancelled_session_saved=false\n";
  }
  // Follow trained processing with an unflagged digital mute and with an
  // in-place bypass, using the same real callback and public parameter input.
  std::array<float, 512> silence{}, mutedOutput{};
  session.process(frames, silence.data(), mutedOutput.data(), 512, false);
  require(std::all_of(mutedOutput.begin(), mutedOutput.end(),
                      [](float sample) { return sample == 0; }),
          "production callback generated audio during mono microphone mute");
  auto bypassed = std::array<float, 512>{};
  for (int i = 0; i < 512; ++i)
    bypassed[i] = voice[i];
  const auto original = bypassed;
  session.process(frames + 512, bypassed.data(), bypassed.data(), 512, false,
                  true);
  require(bypassed == original,
          "production callback failed in-place mono bypass");
}
} // namespace

int main() {
  try {
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    require(SUCCEEDED(apartment), "test host COM initialization failed");
    GUID identifier{};
    require(SUCCEEDED(CoCreateGuid(&identifier)), "test directory GUID failed");
    std::array<wchar_t, 40> identifierText{};
    require(StringFromGUID2(identifier, identifierText.data(),
                            static_cast<int>(identifierText.size())) > 0,
            "test directory GUID formatting failed");
    generated_input::captureRoot =
        std::filesystem::temp_directory_path() /
        (L"buncancel-host-signal-" + std::wstring(identifierText.data()));
    std::filesystem::create_directory(generated_input::captureRoot);
    run(false, false);
    run(false, true);
    run(true, false);
    std::filesystem::remove_all(generated_input::captureRoot);
    CoUninitialize();
    std::cout
        << "Production VST3 callback signal tests passed with generated "
           "capture PCM and clock; physical WASAPI routing is unverified.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "host signal test: " << error.what() << '\n';
    return 1;
  }
}
