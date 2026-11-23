#include "aec.hpp"
#include "diagnostics.hpp"
#include "editor.hpp"
#include "host_clock.hpp"
#include "reference_capture.hpp"
#include "reference_timeline.hpp"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
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
constexpr const char *statusRequest = "BunCancel.Status.Request";
constexpr const char *statusReply = "BunCancel.Status.Reply";
constexpr const char *statusAttribute = "status";

constexpr const char *diagnosticsAttribute = "diagnostics.v1";

// Atomic words avoid the C++ data race that a plain-data seqlock would have.
// One writer publishes, readers make bounded attempts and retain their previous
// observation on contention. Every operation is lock-free on the x64 target.
class DiagnosticSnapshot {
public:
  void publish(const Diagnostics &value) noexcept {
    std::array<std::uint64_t, wordCount> words{};
    std::memcpy(words.data(), &value, sizeof(value));
    sequence_.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t i = 0; i < wordCount; ++i)
      words_[i].store(words[i], std::memory_order_release);
    sequence_.fetch_add(1, std::memory_order_release);
  }

  bool read(Diagnostics &value) const noexcept {
    for (int attempt = 0; attempt < 3; ++attempt) {
      const auto before = sequence_.load(std::memory_order_acquire);
      if (before & 1)
        continue;
      std::array<std::uint64_t, wordCount> words{};
      for (std::size_t i = 0; i < wordCount; ++i)
        words[i] = words_[i].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (before == sequence_.load(std::memory_order_relaxed) && before != 0) {
        std::memcpy(static_cast<void *>(&value), words.data(), sizeof(value));
        return true;
      }
    }
    return false;
  }

private:
  static_assert(std::is_trivially_copyable_v<Diagnostics>);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static constexpr std::size_t wordCount =
      (sizeof(Diagnostics) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
  std::array<std::atomic<std::uint64_t>, wordCount> words_{};
  std::atomic<std::uint64_t> sequence_{};
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
      const auto label = (device.kind == capture::EndpointKind::render_loopback
                              ? "[Render loopback] "
                              : "[Capture] ") +
                         device.name;
      const auto name = StringConvert::convert(label);
      picker->appendString(toTChar(name));
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
    status->appendString(STR16("Dry - no reference selected"));
    status->appendString(STR16("Status unavailable - waiting for processor"));
    status->appendString(STR16("Dry - bypassed"));
    status->appendString(STR16("Ready - reference quiet"));
    status->appendString(STR16("Dry - reference timing interrupted"));
    status->appendString(STR16("Ready - no correlated echo"));
    status->appendString(STR16("Ready - ambiguous echo timing"));
    status->appendString(STR16("Ready - check microphone monitoring"));
    status->appendString(STR16("Ready - echo delay outside supported range"));
    status->appendString(STR16("Ready - aligned and monitoring"));
    status->appendString(STR16("Ready - monitoring"));
    parameters.addParameter(status);
    setParamNormalized(statusId,
                       static_cast<double>(ProcessingStatus::unavailable) /
                           (static_cast<int>(ProcessingStatus::count) - 1));
    return kResultOk;
  }

  IPlugView *PLUGIN_API createView(FIDString name) override {
    if (name && std::strcmp(name, ViewType::kEditor) == 0)
      return createEditor(this);
    return nullptr;
  }

  void refreshStatus() {
    // The native editor calls this only on its UI timer. Hosts need not forward
    // processor output parameters for the custom editor to observe live status.
    // A missed reply expires the previous observation, including proxy
    // failures.
    const auto now = std::chrono::steady_clock::now();
    if (!getPeer() || !haveStatus_ ||
        now - lastStatus_ > std::chrono::seconds(1))
      setParamNormalized(statusId,
                         static_cast<double>(ProcessingStatus::unavailable) /
                             (static_cast<int>(ProcessingStatus::count) - 1));
    if (getPeer())
      sendMessageID(statusRequest);
  }

  tresult PLUGIN_API notify(IMessage *message) override {
    if (message && message->getMessageID() &&
        std::strcmp(message->getMessageID(), statusReply) == 0) {
      if (!getPeer())
        return kResultFalse;
      int64 status = 0;
      if (!message->getAttributes() ||
          message->getAttributes()->getInt(statusAttribute, status) !=
              kResultOk ||
          status < 0 || status >= static_cast<int>(ProcessingStatus::count))
        return kResultFalse;
      const void *binary = nullptr;
      uint32 bytes = 0;
      if (message->getAttributes()->getBinary(diagnosticsAttribute, binary,
                                              bytes) != kResultOk ||
          !binary || bytes != sizeof(Diagnostics))
        return kResultFalse;
      Diagnostics diagnostic;
      std::memcpy(&diagnostic, binary, sizeof(diagnostic));
      if (static_cast<std::uint32_t>(diagnostic.status) >=
              static_cast<std::uint32_t>(ProcessingStatus::count) ||
          static_cast<std::uint32_t>(diagnostic.alignment_reason) >
              static_cast<std::uint32_t>(AlignmentReason::referenceMissing))
        return kResultFalse;
      diagnostics_.publish(diagnostic);
      setParamNormalized(statusId,
                         static_cast<double>(status) /
                             (static_cast<int>(ProcessingStatus::count) - 1));
      haveStatus_ = true;
      lastStatus_ = std::chrono::steady_clock::now();
      return kResultOk;
    }
    return EditController::notify(message);
  }

  tresult PLUGIN_API disconnect(IConnectionPoint *other) override {
    haveStatus_ = false;
    setParamNormalized(statusId,
                       static_cast<double>(ProcessingStatus::unavailable) /
                           (static_cast<int>(ProcessingStatus::count) - 1));
    return EditController::disconnect(other);
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

public:
  bool readDiagnostic(Diagnostics &value) const {
    return haveStatus_ && getPeer() &&
           std::chrono::steady_clock::now() - lastStatus_ <=
               std::chrono::seconds(1) &&
           diagnostics_.read(value);
  }

private:
  DiagnosticSnapshot diagnostics_;
  bool haveStatus_ = false;
  std::chrono::steady_clock::time_point lastStatus_{};
  std::vector<capture::Endpoint> devices_;
};

void refreshStatus(EditController *controller) {
  if (controller)
    static_cast<Controller *>(controller)->refreshStatus();
}

bool readDiagnostics(EditController *controller, Diagnostics &snapshot) {
  return controller &&
         static_cast<Controller *>(controller)->readDiagnostic(snapshot);
}

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
      microphone_.resize(static_cast<size_t>(setup.maxSamplesPerBlock));
      capture::ReferenceTimelineConfig config;
      config.sample_rate = static_cast<std::uint32_t>(setup.sampleRate);
      config.max_block_frames =
          static_cast<std::uint32_t>(setup.maxSamplesPerBlock);
      config.history_frames = static_cast<std::uint32_t>(setup.sampleRate * 2);
      config.holdback_frames =
          static_cast<std::uint32_t>(setup.sampleRate * 0.020);
      timeline_ = std::make_unique<capture::ReferenceTimeline>(config);
    } catch (...) {
      engine_.reset();
      return kOutOfMemory;
    }
    rate_ = static_cast<uint32_t>(setup.sampleRate);
    hostClock_ = HostClock(setup.sampleRate);
#ifdef _WIN32
    LARGE_INTEGER frequency{};
    qpcScale_ = QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0
                    ? 10000000.0 / static_cast<double>(frequency.QuadPart)
                    : 0;
#endif
    return AudioEffect::setupProcessing(setup);
  }

  tresult PLUGIN_API setActive(TBool active) override {
    if (active) {
      if (!engine_)
        return kResultFalse;
      engine_->reset();
      lastReportedStatus_ = -1;
      timeline_->reset();
      hostClock_.reset();
      haveReference_ = false;
      underruns_ = 0;
      dropoutBase_ = capture_.dropped_packets() + capture_.discontinuities();
      rawReferenceRms_ = rawReferencePeak_ = 0;
      rawReferenceQpc_ = 0;
      waitingSinceQpc_ = 0;
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

  tresult PLUGIN_API notify(IMessage *message) override {
    if (message && message->getMessageID() &&
        std::strcmp(message->getMessageID(), statusRequest) == 0) {
      // IConnectionPoint::notify is a UI-thread API. Only atomic observations
      // cross from process(); message allocation and host callbacks stay here.
      Diagnostics diagnostic;
      if (!diagnostics_.read(diagnostic))
        diagnostic.status = ProcessingStatus::unavailable;
      bool staleObservation = false;
#ifdef _WIN32
      LARGE_INTEGER now{};
      if (qpcScale_ > 0 && QueryPerformanceCounter(&now)) {
        const auto currentQpc = static_cast<std::uint64_t>(
            static_cast<double>(now.QuadPart) * qpcScale_);
        staleObservation =
            diagnostic.observation_qpc_100ns &&
            currentQpc > diagnostic.observation_qpc_100ns &&
            currentQpc - diagnostic.observation_qpc_100ns > 10000000;
      }
#endif
      const bool sourceChanged = selected_.load() != connected_.load();
      if (!enabled_.load())
        diagnostic.status = ProcessingStatus::dry;
      if (bypass_.load())
        diagnostic.status = ProcessingStatus::bypassed;
      else if (enabled_.load() && selected_.load() == 0)
        diagnostic.status = ProcessingStatus::noReference;
      else if (enabled_.load() && (!running_.load() || !processing_.load() ||
                                   staleObservation || sourceChanged))
        diagnostic.status = ProcessingStatus::awaitingReference;
      else if (enabled_.load()) {
        switch (capture_.status()) {
        case capture::CaptureStatus::device_error:
          diagnostic.status = ProcessingStatus::deviceError;
          break;
        case capture::CaptureStatus::rate_mismatch:
          diagnostic.status = ProcessingStatus::rateMismatch;
          break;
        case capture::CaptureStatus::unsupported_format:
          diagnostic.status = ProcessingStatus::unsupportedFormat;
          break;
        default:
          break;
        }
      }
      if (!enabled_.load() || bypass_.load())
        diagnostic.aligned = diagnostic.reducing = false;
      if (sourceChanged) {
        diagnostic.reference_available = false;
        diagnostic.reference_rms = diagnostic.reference_peak = 0;
        diagnostic.reference_sample_rate = 0;
        diagnostic.timing_valid = diagnostic.candidate_valid = false;
        diagnostic.candidate_confirmed = false;
        diagnostic.aligned = diagnostic.reducing = false;
      }
      if (!running_.load() || !processing_.load() || staleObservation) {
        diagnostic.mic_available = false;
        diagnostic.reference_available = false;
        diagnostic.timing_valid = false;
        diagnostic.candidate_valid = false;
        diagnostic.aligned = diagnostic.reducing = false;
      }
      auto reply = owned(allocateMessage());
      if (!reply || !reply->getAttributes())
        return kResultFalse;
      reply->setMessageID(statusReply);
      if (reply->getAttributes()->setInt(
              statusAttribute, static_cast<int64>(diagnostic.status)) !=
          kResultOk)
        return kResultFalse;
      if (reply->getAttributes()->setBinary(diagnosticsAttribute, &diagnostic,
                                            sizeof(diagnostic)) != kResultOk)
        return kResultFalse;
      return sendMessage(reply);
    }
    return AudioEffect::notify(message);
  }

  tresult PLUGIN_API setProcessing(TBool state) override {
    processing_.store(state != 0);
    if (!state) {
      hostClock_.reset();
      if (timeline_)
        timeline_->reset();
      if (engine_)
        engine_->reset();
    }
    return kResultOk;
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
      Diagnostics diagnostic;
      diagnostics_.read(diagnostic);
      diagnostic.host_sample_rate = rate_;
      diagnostic.bypassed = bypass_.load();
      if (selected_.load() != audioSelection_) {
        diagnostic.reference_available = false;
        diagnostic.reference_rms = diagnostic.reference_peak = 0;
        diagnostic.reference_sample_rate = 0;
        diagnostic.timing_valid = diagnostic.candidate_valid = false;
        diagnostic.candidate_confirmed = false;
        diagnostic.underruns = diagnostic.dropouts = 0;
      }
      if (!enabled_.load() || bypass_.load())
        diagnostic.aligned = diagnostic.reducing = false;
      if (bypass_.load())
        diagnostic.status = ProcessingStatus::bypassed;
      else if (!enabled_.load())
        diagnostic.status = ProcessingStatus::dry;
      else if (!selected_.load())
        diagnostic.status = ProcessingStatus::noReference;
      publishDiagnostics(data, diagnostic);
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
    const auto frames = static_cast<std::size_t>(data.numSamples);
    const bool fits = engine_ && timeline_ && frames <= reference_.size();
    const bool hostSilent = (data.inputs[0].silenceFlags & 1) != 0;
    Diagnostics diagnostic;
    diagnostic.host_sample_rate = rate_;
    diagnostic.reference_holdback_ms = 20;
    diagnostic.bypassed = bypass_.load();
    diagnostic.mic_available = true;
    double micEnergy = 0;
    for (std::size_t i = 0; i < frames; ++i) {
      const float sample = hostSilent ? 0.0f : mic[i];
      if (fits)
        microphone_[i] = sample;
      if (std::isfinite(sample)) {
        micEnergy += static_cast<double>(sample) * sample;
        diagnostic.mic_peak = std::max(diagnostic.mic_peak, std::abs(sample));
      }
    }
    diagnostic.mic_rms = static_cast<float>(std::sqrt(micEnergy / frames));
    const bool micQuiet = diagnostic.mic_peak == 0;
    const auto qpc = microphoneTime(data);
    diagnostic.observation_qpc_100ns = callbackQpc_;
    const auto connection = captureEpoch_.load();
    const int selected = selected_.load();
    if (connection != audioConnection_ || selected != audioSelection_) {
      if (timeline_)
        timeline_->reset();
      if (engine_)
        engine_->reset();
      audioConnection_ = connection;
      audioSelection_ = selected;
      haveReference_ = false;
      underruns_ = 0;
      dropoutBase_ = capture_.dropped_packets() + capture_.discontinuities();
      rawReferenceRms_ = rawReferencePeak_ = 0;
      rawReferenceQpc_ = 0;
      waitingSinceQpc_ = 0;
    }
    double referenceEnergy = 0;
    std::size_t referenceSamples = 0;
    float referencePeak = 0;
    capture::ReferencePacket packet;
    for (int drained = 0; drained < 64 && capture_.pop(packet); ++drained) {
      // Meter the actual arriving source even when timeline recovery is dry.
      if (packet.channel_count >= 1 && packet.channel_count <= 2 &&
          packet.frame_count <= capture::ReferencePacket::max_frames) {
        const auto samples = packet.frame_count * packet.channel_count;
        for (std::uint32_t i = 0; i < samples; ++i) {
          const float sample = packet.samples[i];
          if (std::isfinite(sample)) {
            referenceEnergy += static_cast<double>(sample) * sample;
            referencePeak = std::max(referencePeak, std::abs(sample));
          }
        }
        referenceSamples += samples;
        rawReferenceQpc_ = packet.qpc_position_100ns;
      }
      if (timeline_)
        timeline_->push(packet);
    }
    if (referenceSamples) {
      rawReferenceRms_ =
          static_cast<float>(std::sqrt(referenceEnergy / referenceSamples));
      rawReferencePeak_ = referencePeak;
    }
    const auto captureStatus = capture_.status();
    const bool connected = selected > 0 && selected == connected_.load() &&
                           captureStatus == capture::CaptureStatus::running;
    diagnostic.reference_available =
        connected && rawReferenceQpc_ && callbackQpc_ &&
        std::abs(static_cast<double>(callbackQpc_) -
                 static_cast<double>(rawReferenceQpc_)) < 1000000;
    diagnostic.reference_rms =
        diagnostic.reference_available ? rawReferenceRms_ : 0;
    diagnostic.reference_peak =
        diagnostic.reference_available ? rawReferencePeak_ : 0;
    diagnostic.reference_sample_rate =
        selected == connected_.load() ? capture_.actual_sample_rate() : 0;
    capture::TimelineResult timing;
    if (fits && connected && qpc)
      timing = timeline_->render(qpc, static_cast<std::uint32_t>(frames),
                                 reference_.data());
    if (timing.reset_required && engine_)
      engine_->reset();
    diagnostic.timing_valid = timing.latest_packet_qpc_100ns != 0 && qpc != 0;
    if (diagnostic.timing_valid)
      diagnostic.timestamp_offset_ms =
          (static_cast<double>(qpc) -
           static_cast<double>(timing.latest_packet_qpc_100ns)) /
          10000;
    const bool enabled = enabled_.load() && !bypass_.load();
    const bool complete = fits && connected && timing.usable;
    if (connected && !waitingSinceQpc_)
      waitingSinceQpc_ = callbackQpc_;
    const bool prefillExpired = waitingSinceQpc_ &&
                                callbackQpc_ > waitingSinceQpc_ &&
                                callbackQpc_ - waitingSinceQpc_ > 1000000;
    const bool interrupted =
        connected && !complete && (haveReference_ || prefillExpired);
    if (complete)
      haveReference_ = true;
    else if (interrupted)
      ++underruns_;
    if (fits) {
      engine_->process(microphone_.data(),
                       complete && enabled ? reference_.data() : nullptr, out,
                       data.numSamples);
    } else if (hostSilent) {
      std::fill_n(out, frames, 0.0f);
    } else {
      std::copy_n(mic, frames, out);
    }
    // Bypass and missing-reference paths preserve the raw microphone exactly.
    // The DSP still observes the missing reference to invalidate old
    // adaptation.
    if ((!complete || !enabled) && !hostSilent)
      std::copy_n(mic, frames, out);
    // AEC must never synthesize output when the microphone is actually silent.
    if (micQuiet)
      std::fill_n(out, frames, 0.0f);
    data.outputs[0].silenceFlags = micQuiet ? 1 : 0;
    if (engine_) {
      const auto alignment = engine_->diagnostics();
      diagnostic.alignment_reason =
          static_cast<AlignmentReason>(alignment.state);
      diagnostic.candidate_valid = alignment.peakValid;
      diagnostic.candidate_confirmed = alignment.candidateValid;
      const auto observedLag = alignment.candidateValid
                                   ? alignment.candidateLagSamples
                                   : alignment.peakLagSamples;
      diagnostic.candidate_delay_ms = 1000.0 * observedLag / rate_ + 20;
      diagnostic.correlation = alignment.peakCorrelation;
      diagnostic.aligned = complete && enabled && engine_->aligned();
      diagnostic.reduction_db = alignment.reductionDb;
      diagnostic.reducing = diagnostic.aligned && alignment.reductionValid &&
                            alignment.reductionDb > 0 && !micQuiet;
    }
    diagnostic.underruns = underruns_;
    const auto totalDropouts =
        capture_.dropped_packets() + capture_.discontinuities();
    diagnostic.dropouts =
        totalDropouts >= dropoutBase_ ? totalDropouts - dropoutBase_ : 0;
    ProcessingObservation observation;
    observation.enabled = enabled_.load();
    observation.bypassed = bypass_.load();
    observation.selected = selected != 0;
    observation.connected = connected;
    observation.processing = processing_.load();
    observation.format_supported =
        captureStatus != capture::CaptureStatus::unsupported_format;
    observation.rate_matched =
        captureStatus != capture::CaptureStatus::rate_mismatch;
    observation.device_error =
        captureStatus == capture::CaptureStatus::device_error;
    observation.interrupted = interrupted;
    observation.reducing = diagnostic.reducing;
    diagnostic.status = selectProcessingStatus(observation);
    publishDiagnostics(data, diagnostic);
    return kResultOk;
  }

private:
  std::uint64_t microphoneTime(const ProcessData &data) noexcept {
    std::uint64_t now = 0;
#ifdef _WIN32
    LARGE_INTEGER counter{};
    if (qpcScale_ > 0 && QueryPerformanceCounter(&counter))
      now = static_cast<std::uint64_t>(static_cast<double>(counter.QuadPart) *
                                       qpcScale_);
#endif
    callbackQpc_ = now;
    if (!now)
      return 0;
    const bool continuousValid =
        data.processContext &&
        (data.processContext->state & ProcessContext::kContTimeValid);
    const auto timing = hostClock_.observe(
        now, static_cast<std::uint32_t>(data.numSamples), continuousValid,
        continuousValid ? data.processContext->continousTimeSamples : 0);
    if (timing.reset_required) {
      if (timeline_)
        timeline_->reset();
      if (engine_)
        engine_->reset();
      haveReference_ = false;
    }
    return timing.microphone_start_qpc_100ns;
  }

  void publishDiagnostics(ProcessData &data,
                          const Diagnostics &diagnostic) noexcept {
    diagnostics_.publish(diagnostic);
    const auto value = static_cast<int>(diagnostic.status);
    if (!data.outputParameterChanges || value == lastReportedStatus_)
      return;
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
  std::vector<float> reference_, microphone_;
  std::unique_ptr<capture::ReferenceTimeline> timeline_;
  DiagnosticSnapshot diagnostics_;
  int lastReportedStatus_ = -1;
  std::atomic<bool> processing_{false};
  std::uint64_t audioConnection_ = 0;
  int audioSelection_ = 0;
  std::atomic<std::uint64_t> captureEpoch_{0};
  std::uint64_t underruns_ = 0, dropoutBase_ = 0, rawReferenceQpc_ = 0;
  std::uint64_t callbackQpc_ = 0, waitingSinceQpc_ = 0;
  float rawReferenceRms_ = 0, rawReferencePeak_ = 0;
  bool haveReference_ = false;
  HostClock hostClock_;
  double qpcScale_ = 0;
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
           "BunCancel Experimental", 0, "Fx|Restoration", "0.1.2",
           kVstVersionString, buncancel::plugin::Processor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(buncancel::plugin::controllerId),
           PClassInfo::kManyInstances, kVstComponentControllerClass,
           "BunCancel Controller", 0, "", "0.1.2", kVstVersionString,
           buncancel::plugin::Controller::create)
END_FACTORY
