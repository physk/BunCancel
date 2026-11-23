#include "aec.hpp"
#include "reference_capture.hpp"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace buncancel::plugin {
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace capture = buncancel::capture;

const FUID processorId(0x4D79F4D0, 0xF7794F7A, 0xB9A129E3, 0x438A692C);
const FUID controllerId(0x79320171, 0x14FC4B9B, 0xA9C3421A, 0x980C6D50);
constexpr ParamID enabledId = 0;
constexpr ParamID endpointId = 1;
constexpr ParamID bypassId = 2;
constexpr ParamID statusId = 3;

enum class ProcessingStatus {
  dry,
  awaitingReference,
  rateMismatch,
  unsupportedFormat,
  deviceError,
  acquiringAlignment,
  cancelling,
  silentMicrophone,
  count
};

std::vector<capture::Endpoint> endpoints() {
  // One immutable module snapshot guarantees identical controller/processor
  // parameter indices even if Windows topology changes between initialization.
  static const auto snapshot = [] {
    auto result = capture::ReferenceCapture::enumerate_endpoints();
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.id < b.id; });
    return result;
  }();
  return snapshot;
}

struct Settings {
  bool enabled = false;
  bool bypass = false;
  std::string endpoint;
};

bool readSettings(IBStream *state, Settings &settings) {
  if (!state)
    return false;
  IBStreamer stream(state, kLittleEndian);
  int32 version = 0, enabled = 0, bypass = 0, length = 0;
  if (!stream.readInt32(version) || version != 1 ||
      !stream.readInt32(enabled) || !stream.readInt32(bypass) ||
      !stream.readInt32(length) || length < 0 || length > 4096)
    return false;
  settings.endpoint.resize(static_cast<size_t>(length));
  if (length && stream.readRaw(settings.endpoint.data(), length) != length)
    return false;
  settings.enabled = enabled != 0;
  settings.bypass = bypass != 0;
  return true;
}

bool writeSettings(IBStream *state, const Settings &settings) {
  if (!state || settings.endpoint.size() > 4096)
    return false;
  IBStreamer stream(state, kLittleEndian);
  const auto length = static_cast<int32>(settings.endpoint.size());
  return stream.writeInt32(1) && stream.writeInt32(settings.enabled) &&
         stream.writeInt32(settings.bypass) && stream.writeInt32(length) &&
         (!length ||
          stream.writeRaw(settings.endpoint.data(), length) == length);
}

int selection(const std::vector<capture::Endpoint> &list,
              const std::string &id) {
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i].id == id)
      return static_cast<int>(i) + 1;
  return 0;
}

class Controller final : public EditController {
public:
  static FUnknown *create(void *) {
    return static_cast<IEditController *>(new Controller);
  }

  tresult PLUGIN_API initialize(FUnknown *context) override {
    auto result = EditController::initialize(context);
    if (result != kResultOk)
      return result;
    devices_ = endpoints();
    parameters.addParameter(STR16("Enable experimental AEC"), nullptr, 1, 0,
                            ParameterInfo::kCanAutomate, enabledId);
    parameters.addParameter(
        STR16("Bypass"), nullptr, 1, 0,
        ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, bypassId);
    auto *picker =
        new StringListParameter(STR16("Playback reference (no mic monitoring)"),
                                endpointId, nullptr, ParameterInfo::kIsList);
    picker->appendString(STR16("None - dry microphone"));
    for (const auto &device : devices_) {
      String128 name{};
      const auto label =
          device.name + (device.kind == capture::EndpointKind::render_loopback
                             ? " [render loopback]"
                             : " [capture]");
      StringConvert::convert(label, name);
      picker->appendString(name);
    }
    parameters.addParameter(picker);
    auto *status = new StringListParameter(
        STR16("Reference / AEC status"), statusId, nullptr,
        ParameterInfo::kIsList | ParameterInfo::kIsReadOnly);
    status->appendString(STR16("Dry - AEC off or bypassed"));
    status->appendString(STR16("Dry - awaiting playback reference"));
    status->appendString(STR16("Dry - reference sample-rate mismatch"));
    status->appendString(STR16("Dry - unsupported reference format"));
    status->appendString(STR16("Dry - reference device unavailable"));
    status->appendString(STR16("Dry - acquiring alignment"));
    status->appendString(STR16("Cancelling - alignment acquired"));
    status->appendString(STR16("Dry - silent microphone"));
    parameters.addParameter(status);
    return kResultOk;
  }

  tresult PLUGIN_API setComponentState(IBStream *state) override {
    Settings settings;
    if (!readSettings(state, settings))
      return kResultFalse;
    setParamNormalized(enabledId, settings.enabled ? 1 : 0);
    setParamNormalized(bypassId, settings.bypass ? 1 : 0);
    setParamNormalized(endpointId, devices_.empty()
                                       ? 0
                                       : static_cast<double>(selection(
                                             devices_, settings.endpoint)) /
                                             devices_.size());
    return kResultOk;
  }

private:
  std::vector<capture::Endpoint> devices_;
};

class Processor final : public AudioEffect {
public:
  Processor() { setControllerClass(controllerId); }
  ~Processor() override { stopWorker(); }
  static FUnknown *create(void *) {
    return static_cast<IAudioProcessor *>(new Processor);
  }

  tresult PLUGIN_API initialize(FUnknown *context) override {
    const auto result = AudioEffect::initialize(context);
    if (result != kResultOk)
      return result;
    addAudioInput(STR16("Microphone"), SpeakerArr::kMono);
    addAudioOutput(STR16("Clean microphone"), SpeakerArr::kMono);
    devices_ = endpoints();
    return kResultOk;
  }

  tresult PLUGIN_API terminate() override {
    stopWorker();
    return AudioEffect::terminate();
  }

  tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs,
                                        int32 numInputs,
                                        SpeakerArrangement *outputs,
                                        int32 numOutputs) override {
    if (numInputs != 1 || numOutputs != 1 || inputs[0] != SpeakerArr::kMono ||
        outputs[0] != SpeakerArr::kMono)
      return kResultFalse;
    return AudioEffect::setBusArrangements(inputs, numInputs, outputs,
                                           numOutputs);
  }

  tresult PLUGIN_API canProcessSampleSize(int32 format) override {
    return format == kSample32 ? kResultTrue : kResultFalse;
  }

  tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
    if (setup.symbolicSampleSize != kSample32 || setup.sampleRate < 8000 ||
        setup.sampleRate > 192000 || !std::isfinite(setup.sampleRate) ||
        setup.maxSamplesPerBlock < 1)
      return kResultFalse;
    stopWorker();
    try {
      engine_ =
          std::make_unique<buncancel::Aec>(static_cast<int>(setup.sampleRate));
      reference_.resize(static_cast<size_t>(setup.maxSamplesPerBlock));
    } catch (...) {
      engine_.reset();
      return kOutOfMemory;
    }
    rate_ = static_cast<uint32_t>(setup.sampleRate);
    return AudioEffect::setupProcessing(setup);
  }

  tresult PLUGIN_API setActive(TBool active) override {
    if (active) {
      if (!engine_)
        return kResultFalse;
      engine_->reset();
      lastReportedStatus_ = -1;
      packetOffset_ = packet_.frame_count = 0;
      stopWorker();
      running_.store(true);
      try {
        worker_ = std::thread([this] { controlLoop(); });
      } catch (...) {
        running_.store(false);
        return kResultFalse;
      }
    } else {
      stopWorker();
    }
    return AudioEffect::setActive(active);
  }

  uint32 PLUGIN_API getLatencySamples() override { return 0; }

  tresult PLUGIN_API setState(IBStream *state) override {
    Settings settings;
    if (!readSettings(state, settings))
      return kResultFalse;
    selected_.store(selection(devices_, settings.endpoint));
    enabled_.store(settings.enabled);
    bypass_.store(settings.bypass);
    return kResultOk;
  }

  tresult PLUGIN_API getState(IBStream *state) override {
    Settings settings;
    settings.enabled = enabled_.load();
    settings.bypass = bypass_.load();
    const int selected = selected_.load();
    if (selected > 0 && selected <= static_cast<int>(devices_.size()))
      settings.endpoint = devices_[static_cast<size_t>(selected - 1)].id;
    return writeSettings(state, settings) ? kResultOk : kResultFalse;
  }

  tresult PLUGIN_API process(ProcessData &data) override {
    if (data.inputParameterChanges) {
      for (int32 i = 0; i < data.inputParameterChanges->getParameterCount();
           ++i) {
        auto *queue = data.inputParameterChanges->getParameterData(i);
        if (!queue || queue->getPointCount() == 0)
          continue;
        int32 offset = 0;
        ParamValue value = 0;
        if (queue->getPoint(queue->getPointCount() - 1, offset, value) !=
                kResultTrue ||
            !std::isfinite(value))
          continue;
        value = std::clamp(value, 0.0, 1.0);
        switch (queue->getParameterId()) {
        case enabledId:
          enabled_.store(value >= 0.5);
          break;
        case bypassId:
          bypass_.store(value >= 0.5);
          break;
        case endpointId:
          selected_.store(
              static_cast<int>(std::lround(value * devices_.size())));
          break;
        }
      }
    }
    if (data.numSamples == 0 || data.numInputs == 0 || data.numOutputs == 0) {
      reportStatus(data, false);
      return kResultOk;
    }
    if (data.symbolicSampleSize != kSample32 || data.numInputs < 1 ||
        data.numOutputs < 1 || data.inputs[0].numChannels < 1 ||
        data.outputs[0].numChannels < 1 || !data.inputs[0].channelBuffers32 ||
        !data.outputs[0].channelBuffers32)
      return kResultFalse;
    const float *mic = data.inputs[0].channelBuffers32[0];
    float *out = data.outputs[0].channelBuffers32[0];
    if (!mic || !out || data.numSamples < 0)
      return kResultFalse;
    if (data.inputs[0].silenceFlags & 1) {
      std::fill_n(out, data.numSamples, 0.0f);
      if (engine_ && !silent_)
        engine_->reset();
      silent_ = true;
      packetOffset_ = packet_.frame_count = 0;
      capture::ReferencePacket discarded;
      for (int i = 0; i < 64 && capture_.pop(discarded); ++i) {
      }
      data.outputs[0].silenceFlags = 1;
      reportStatus(data, false, true);
      return kResultOk;
    }
    silent_ = false;
    const bool usable =
        engine_ && enabled_.load() && !bypass_.load() &&
        selected_.load() == connected_.load() && connected_.load() > 0 &&
        capture_.status() == capture::CaptureStatus::running &&
        static_cast<size_t>(data.numSamples) <= reference_.size();
    if (!usable) {
      packetOffset_ = packet_.frame_count = 0;
    }
    const auto connection = captureEpoch_.load();
    if (connection != audioConnection_) {
      packetOffset_ = packet_.frame_count = 0;
      if (engine_)
        engine_->reset();
      audioConnection_ = connection;
    }
    bool complete = usable;
    for (int32 i = 0; complete && i < data.numSamples; ++i) {
      if (packetOffset_ >= packet_.frame_count) {
        if (!capture_.pop(packet_) || packet_.discontinuity ||
            packet_.sample_rate != rate_ || packet_.channel_count < 1 ||
            packet_.channel_count > 2 || packet_.frame_count == 0 ||
            packet_.frame_count > 512 ||
            (clock100ns_.load() > packet_.qpc_position_100ns &&
             clock100ns_.load() - packet_.qpc_position_100ns > 1000000)) {
          complete = false;
          packetOffset_ = packet_.frame_count = 0;
          break;
        }
        packetOffset_ = 0;
        if (packet_.generation != audioGeneration_ ||
            (havePosition_ && packet_.device_position != expectedPosition_)) {
          engine_->reset();
          audioGeneration_ = packet_.generation;
        }
        expectedPosition_ = packet_.device_position + packet_.frame_count;
        havePosition_ = true;
      }
      const auto index = packetOffset_++ * packet_.channel_count;
      reference_[static_cast<size_t>(i)] =
          packet_.channel_count == 2
              ? 0.5f * (packet_.samples[index] + packet_.samples[index + 1])
              : packet_.samples[index];
    }
    if (complete) {
      engine_->process(mic, reference_.data(), out, data.numSamples);
    } else if (engine_ &&
               static_cast<size_t>(data.numSamples) <= reference_.size()) {
      engine_->process(mic, nullptr, out, data.numSamples);
    } else {
      std::copy_n(mic, data.numSamples, out);
    }
    data.outputs[0].silenceFlags = data.inputs[0].silenceFlags;
    reportStatus(data, complete);
    return kResultOk;
  }

private:
  void reportStatus(ProcessData &data, bool processed,
                    bool silent = false) noexcept {
    auto status = ProcessingStatus::awaitingReference;
    if (!enabled_.load() || bypass_.load()) {
      status = ProcessingStatus::dry;
    } else {
      switch (capture_.status()) {
      case capture::CaptureStatus::rate_mismatch:
        status = ProcessingStatus::rateMismatch;
        break;
      case capture::CaptureStatus::unsupported_format:
        status = ProcessingStatus::unsupportedFormat;
        break;
      case capture::CaptureStatus::device_error:
        status = ProcessingStatus::deviceError;
        break;
      case capture::CaptureStatus::running:
        if (selected_.load() == connected_.load() && connected_.load() > 0) {
          status = silent      ? ProcessingStatus::silentMicrophone
                   : processed ? (engine_ && engine_->aligned()
                                      ? ProcessingStatus::cancelling
                                      : ProcessingStatus::acquiringAlignment)
                               : ProcessingStatus::awaitingReference;
        }
        break;
      default:
        break;
      }
    }
    const auto value = static_cast<int>(status);
    if (!data.outputParameterChanges || value == lastReportedStatus_)
      return;
    // The host owns the output-change storage supplied for this callback.
    // No SDK parameter objects or strings are created on the audio thread.
    int32 queueIndex = 0;
    auto *queue =
        data.outputParameterChanges->addParameterData(statusId, queueIndex);
    if (!queue)
      return;
    int32 pointIndex = 0;
    const auto normalized = static_cast<double>(value) /
                            (static_cast<int>(ProcessingStatus::count) - 1);
    if (queue->addPoint(0, normalized, pointIndex) == kResultTrue)
      lastReportedStatus_ = value;
  }

  void stopWorker() noexcept {
    running_.store(false);
    if (worker_.joinable())
      worker_.join();
    capture_.stop();
    connected_.store(0);
  }

  void controlLoop() noexcept {
    int previous = -1;
    while (running_.load()) {
#ifdef _WIN32
      LARGE_INTEGER counter{}, frequency{};
      if (QueryPerformanceCounter(&counter) &&
          QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        clock100ns_.store(
            static_cast<uint64_t>(static_cast<long double>(counter.QuadPart) *
                                  10000000.0L / frequency.QuadPart));
      }
#endif
      const int requested =
          enabled_.load() && !bypass_.load() ? selected_.load() : 0;
      if (requested != previous) {
        connected_.store(0);
        capture_.stop();
        captureEpoch_.fetch_add(1);
        if (requested > 0 && requested <= static_cast<int>(devices_.size())) {
          const auto &device = devices_[static_cast<size_t>(requested - 1)];
          try {
            if (capture_.start({device.id, device.kind, rate_}))
              connected_.store(requested);
          } catch (...) {
            capture_.stop();
          }
        }
        previous = requested;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    capture_.stop();
    connected_.store(0);
  }

  std::vector<capture::Endpoint> devices_;
  capture::ReferenceCapture capture_;
  std::unique_ptr<buncancel::Aec> engine_;
  std::vector<float> reference_;
  capture::ReferencePacket packet_{};
  uint32_t packetOffset_ = 0;
  uint64_t audioGeneration_ = 0, expectedPosition_ = 0;
  bool havePosition_ = false;
  bool silent_ = false;
  int lastReportedStatus_ = -1;
  std::atomic<uint64_t> clock100ns_{0};
  uint64_t audioConnection_ = 0;
  std::atomic<uint64_t> captureEpoch_{0};
  uint32_t rate_ = 48000;
  std::atomic<int> selected_{0}, connected_{0};
  std::atomic<bool> enabled_{false}, bypass_{false}, running_{false};
  std::thread worker_;
};
} // namespace buncancel::plugin

bool InitModule() { return true; }
bool DeinitModule() { return true; }

using namespace Steinberg;
using namespace Steinberg::Vst;
BEGIN_FACTORY_DEF("BunCancel", "", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(buncancel::plugin::processorId),
           PClassInfo::kManyInstances, kVstAudioEffectClass,
           "BunCancel Experimental", 0, "Fx|Restoration", "0.1.0",
           kVstVersionString, buncancel::plugin::Processor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(buncancel::plugin::controllerId),
           PClassInfo::kManyInstances, kVstComponentControllerClass,
           "BunCancel Controller", 0, "", "0.1.0", kVstVersionString,
           buncancel::plugin::Controller::create)
END_FACTORY
