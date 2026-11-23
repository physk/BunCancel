// A minimal Windows host for the exact packaged module supplied on the command
// line.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <windows.h>

#include "base/source/fstreamer.h"
#include "diagnostics.hpp"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
constexpr ParamID enabledId = 0, endpointId = 1, bypassId = 2, statusId = 3;
// These are the public native editor automation IDs; no implementation is
// linked.
constexpr int enableControlId = 1001, endpointControlId = 1002;
constexpr int bypassControlId = 1003, statusControlId = 1004;
constexpr int diagnosticsControlId = 1005, actionControlId = 1006;
constexpr int captureControlId = 1007, cancelCaptureControlId = 1008;
constexpr int captureStatusControlId = 1009;

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

class Frame final : public IPlugFrame {
public:
  explicit Frame(HWND host) : host_(host) {}
  unsigned resizeRequests = 0;
  tresult PLUGIN_API queryInterface(const TUID iid, void **object) override {
    *object = nullptr;
    if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
      *object = static_cast<IPlugFrame *>(this);
      addRef();
      return kResultOk;
    }
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return ++references_; }
  uint32 PLUGIN_API release() override { return --references_; }
  tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *size) override {
    if (!view || !size || size->getWidth() <= 0 || size->getHeight() <= 0)
      return kInvalidArgument;
    RECT outer{0, 0, size->getWidth(), size->getHeight()};
    if (!AdjustWindowRectEx(&outer, GetWindowLongW(host_, GWL_STYLE), FALSE,
                            GetWindowLongW(host_, GWL_EXSTYLE)) ||
        !SetWindowPos(host_, nullptr, 0, 0, outer.right - outer.left,
                      outer.bottom - outer.top,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
      return kResultFalse;
    ++resizeRequests;
    return view->onSize(size);
  }

private:
  HWND host_;
  uint32 references_ = 1;
};

class Handler final : public IComponentHandler {
public:
  struct Event {
    char kind;
    ParamID id;
    ParamValue value;
  };
  std::vector<Event> events;
  IAudioProcessor *processor = nullptr;
  tresult PLUGIN_API queryInterface(const TUID iid, void **object) override {
    *object = nullptr;
    if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
      *object = static_cast<IComponentHandler *>(this);
      addRef();
      return kResultOk;
    }
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return ++references_; }
  uint32 PLUGIN_API release() override { return --references_; }
  tresult PLUGIN_API beginEdit(ParamID id) override {
    events.push_back({'b', id, 0});
    return kResultOk;
  }
  tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
    events.push_back({'p', id, value});
    ParameterChanges changes(1);
    int32 index = 0;
    changes.addParameterData(id, index)->addPoint(0, value, index);
    ProcessData flush{};
    flush.symbolicSampleSize = kSample32;
    flush.inputParameterChanges = &changes;
    return processor->process(flush);
  }
  tresult PLUGIN_API endEdit(ParamID id) override {
    events.push_back({'e', id, 0});
    return kResultOk;
  }
  tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
  void expect(ParamID id, ParamValue value) {
    require(events.size() == 3,
            "one user action must emit exactly one gesture");
    require(events[0].kind == 'b' && events[1].kind == 'p' &&
                events[2].kind == 'e',
            "gesture must begin, perform, then end");
    for (const auto &event : events)
      require(event.id == id, "gesture used the wrong parameter");
    require(events[1].value == value, "gesture used the wrong value");
    events.clear();
  }

private:
  uint32 references_ = 1;
};

// Supplies explicit UI observations without replacing the real processor tests.
class DiagnosticPeer final : public IConnectionPoint {
public:
  explicit DiagnosticPeer(IConnectionPoint *controller)
      : controller_(controller) {}
  buncancel::plugin::Diagnostics observation;
  unsigned captureRequests = 0, cancelRequests = 0;
  tresult PLUGIN_API queryInterface(const TUID iid, void **object) override {
    *object = nullptr;
    if (FUnknownPrivate::iidEqual(iid, IConnectionPoint::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
      *object = static_cast<IConnectionPoint *>(this);
      addRef();
      return kResultOk;
    }
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return ++references_; }
  uint32 PLUGIN_API release() override { return --references_; }
  tresult PLUGIN_API connect(IConnectionPoint *) override { return kResultOk; }
  tresult PLUGIN_API disconnect(IConnectionPoint *) override {
    return kResultOk;
  }
  tresult PLUGIN_API notify(IMessage *request) override {
    if (!request || !request->getMessageID())
      return kResultFalse;
    if (std::strcmp(request->getMessageID(), "BunCancel.Capture.Start") == 0) {
      ++captureRequests;
      observation.capture_state =
          buncancel::plugin::LocalCaptureState::recording;
      observation.capture_frames = 48000;
      observation.capture_total_frames = 144000;
      return kResultOk;
    }
    if (std::strcmp(request->getMessageID(), "BunCancel.Capture.Cancel") == 0) {
      ++cancelRequests;
      observation.capture_state =
          buncancel::plugin::LocalCaptureState::cancelled;
      return kResultOk;
    }
    if (std::strcmp(request->getMessageID(), "BunCancel.Status.Request") != 0)
      return kResultFalse;
    HostMessage reply;
    reply.setMessageID("BunCancel.Status.Reply");
    reply.getAttributes()->setInt("status",
                                  static_cast<int64>(observation.status));
    reply.getAttributes()->setBinary("diagnostics.v2", &observation,
                                     sizeof(observation));
    if (observation.capture_state ==
        buncancel::plugin::LocalCaptureState::saved)
      reply.getAttributes()->setString("capture.result",
                                       STR16("C:\\BunCancelTest\\capture-001"));
    return controller_->notify(&reply);
  }

private:
  IConnectionPoint *controller_;
  uint32 references_ = 1;
};

int consentResponse = IDNO;
bool consentShown = false;
std::wstring consentContents;
LRESULT CALLBACK consentHook(int code, WPARAM parameter, LPARAM context) {
  if (code == HCBT_ACTIVATE) {
    const auto window = reinterpret_cast<HWND>(parameter);
    std::array<wchar_t, 128> title{};
    GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (std::wstring(title.data()) == L"Record local diagnostic?") {
      consentShown = true;
      consentContents.clear();
      EnumChildWindows(
          window,
          [](HWND child, LPARAM) -> BOOL {
            std::array<wchar_t, 4096> label{};
            GetWindowTextW(child, label.data(), static_cast<int>(label.size()));
            consentContents += label.data();
            return TRUE;
          },
          0);
      PostMessageW(window, WM_COMMAND, consentResponse, 0);
    }
  }
  return CallNextHookEx(nullptr, code, parameter, context);
}

void respondToConsent(HWND capture, int response) {
  consentResponse = response;
  consentShown = false;
  const auto hook =
      SetWindowsHookExW(WH_CBT, consentHook, nullptr, GetCurrentThreadId());
  require(hook != nullptr, "cannot install capture consent test hook");
  SendMessageW(capture, BM_CLICK, 0, 0);
  UnhookWindowsHookEx(hook);
  require(consentShown, "capture started without a visible consent dialog");
  require(consentContents.find(L"next 3 seconds") != std::wstring::npos &&
              consentContents.find(
                  L"microphone, playback reference and processed output") !=
                  std::wstring::npos &&
              consentContents.find(L"never uploaded") != std::wstring::npos,
          "capture consent omitted duration, signals or local-only behavior");
}

void pump() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

HWND findControl(HWND parent, int id) {
  struct Search {
    int id;
    HWND result;
  } search{id, nullptr};
  EnumChildWindows(
      parent,
      [](HWND window, LPARAM context) -> BOOL {
        auto &search = *reinterpret_cast<Search *>(context);
        if (GetDlgCtrlID(window) == search.id) {
          search.result = window;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  require(search.result != nullptr, "editor control is missing");
  require(IsWindowVisible(search.result), "editor control is hidden");
  return search.result;
}

std::wstring text(HWND window) {
  std::array<wchar_t, 4096> buffer{};
  GetWindowTextW(window, buffer.data(), static_cast<int>(buffer.size()));
  return buffer.data();
}

void refresh(HWND control) {
  // Refresh the real editor timer synchronously, independent of CI scheduling.
  SendMessageW(GetParent(control), WM_TIMER, 1, 0);
  pump();
}

void missingDeviceState(MemoryStream &stream) {
  IBStreamer writer(&stream, kLittleEndian);
  const std::string missing = "buncancel-test-unavailable-endpoint";
  require(
      writer.writeInt32(1) && writer.writeInt32(1) && writer.writeInt32(0) &&
          writer.writeInt32(static_cast<int32>(missing.size())) &&
          writer.writeRaw(missing.data(), static_cast<int32>(missing.size())) ==
              static_cast<int32>(missing.size()),
      "cannot construct missing-device persisted state");
  stream.seek(0, IBStream::kIBSeekSet, nullptr);
}

void dryProcessor(IComponent *component, IAudioProcessor *processor,
                  IEditController *controller) {
  MemoryStream missing;
  missingDeviceState(missing);
  require(component->setState(&missing) == kResultOk,
          "processor rejected missing-device state");
  missing.seek(0, IBStream::kIBSeekSet, nullptr);
  require(controller->setComponentState(&missing) == kResultOk,
          "controller rejected missing-device state");
  require(controller->getParamNormalized(endpointId) == 0,
          "missing endpoint must fall back to None");
  ProcessSetup setup{};
  setup.processMode = kRealtime;
  setup.symbolicSampleSize = kSample32;
  setup.maxSamplesPerBlock = 128;
  setup.sampleRate = 48000;
  require(processor->setupProcessing(setup) == kResultOk,
          "processor setup failed");
  require(component->activateBus(kAudio, kInput, 0, true) == kResultOk &&
              component->activateBus(kAudio, kOutput, 0, true) == kResultOk,
          "mono bus activation failed");
  require(component->setActive(true) == kResultOk,
          "processor activation failed");
  const auto processingResult = processor->setProcessing(true);
  require(processingResult == kResultOk || processingResult == kNotImplemented,
          "processor processing activation failed");
  std::array<float, 128> mic{}, output{};
  for (size_t i = 0; i < mic.size(); ++i)
    mic[i] = static_cast<float>(static_cast<int>(i % 19) - 9) / 16.0f;
  float *inputChannel = mic.data(), *outputChannel = output.data();
  AudioBusBuffers input{}, out{};
  input.numChannels = out.numChannels = 1;
  input.channelBuffers32 = &inputChannel;
  out.channelBuffers32 = &outputChannel;
  ParameterChanges statuses(1);
  ProcessData data{};
  data.processMode = kRealtime;
  data.symbolicSampleSize = kSample32;
  data.numSamples = static_cast<int32>(mic.size());
  data.numInputs = data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &out;
  data.outputParameterChanges = &statuses;
  require(processor->process(data) == kResultOk, "dry processing failed");
  require(mic == output, "missing reference changed the microphone samples");
  require(processor->getLatencySamples() == 0, "unexpected reported latency");
  require(statuses.getParameterCount() == 1, "processor did not report status");
  auto *queue = statuses.getParameterData(0);
  int32 offset = -1;
  ParamValue value = 0;
  require(queue && queue->getParameterId() == statusId &&
              queue->getPoint(0, offset, value) == kResultOk && value > 0,
          "missing reference status is missing");
  controller->setParamNormalized(statusId, value);
  outputChannel = mic.data();
  const auto originalMic = mic;
  require(processor->process(data) == kResultOk && mic == originalMic,
          "in-place missing-reference processing changed microphone samples");

  MemoryStream saved;
  require(component->getState(&saved) == kResultOk, "state save failed");
  saved.seek(0, IBStream::kIBSeekSet, nullptr);
  require(controller->setComponentState(&saved) == kResultOk &&
              controller->getParamNormalized(enabledId) == 1 &&
              controller->getParamNormalized(bypassId) == 0 &&
              controller->getParamNormalized(endpointId) == 0,
          "processor state did not preserve dry configuration");
}

void silentDryBlock(IAudioProcessor *processor) {
  std::array<float, 128> mic{}, output{};
  output.fill(0.25f);
  float *inputChannel = mic.data(), *outputChannel = output.data();
  AudioBusBuffers input{}, out{};
  input.numChannels = out.numChannels = 1;
  input.channelBuffers32 = &inputChannel;
  out.channelBuffers32 = &outputChannel;
  ProcessData data{};
  data.processMode = kRealtime;
  data.symbolicSampleSize = kSample32;
  data.numSamples = static_cast<int32>(mic.size());
  data.numInputs = data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &out;
  require(processor->process(data) == kResultOk && mic == output,
          "silent missing-reference block was not dry");
}

void editor(IComponent *component, IEditController *controller,
            Handler &handler, HWND host, IConnectionPoint *controllerConnection,
            IConnectionPoint *processorConnection) {
  auto *view = controller->createView(ViewType::kEditor);
  require(view != nullptr, "createView(editor) returned null");
  require(view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultOk,
          "editor does not support HWND");
  ViewRect size{};
  require(view->getSize(&size) == kResultOk && size.getWidth() >= 560 &&
              size.getHeight() >= 380,
          "editor returned invalid initial size");
  Frame frame(host);
  RECT initialHost{};
  GetClientRect(host, &initialHost);
  require(initialHost.right < 100 && initialHost.bottom < 100,
          "regression host must start too small for the editor");
  ViewRect tiny(0, 0, 32, 32);
  require(view->onSize(&tiny) == kResultOk &&
              view->setFrame(&frame) == kResultOk,
          "tiny host/frame setup failed");
  require(view->attached(host, kPlatformTypeHWND) == kResultOk,
          "editor attach failed");
  RECT resizedHost{};
  GetClientRect(host, &resizedHost);
  require(frame.resizeRequests > 0 && resizedHost.right >= 560 &&
              resizedHost.bottom >= 380 && view->getSize(&size) == kResultOk &&
              size.getWidth() == resizedHost.right &&
              size.getHeight() == resizedHost.bottom,
          "editor did not request and honor a readable host size");
  pump();
  const HWND enable = findControl(host, enableControlId);
  const HWND endpoint = findControl(host, endpointControlId);
  const HWND bypass = findControl(host, bypassControlId);
  const HWND status = findControl(host, statusControlId);
  const HWND diagnostics = findControl(host, diagnosticsControlId);
  const HWND action = findControl(host, actionControlId);
  const HWND capture = findControl(host, captureControlId);
  const HWND cancelCapture = findControl(host, cancelCaptureControlId);
  const HWND captureStatus = findControl(host, captureStatusControlId);
  require(text(enable) == L"Enable experimental AEC", "enable label is wrong");
  require(text(bypass) == L"Bypass (dry microphone)", "bypass label is wrong");
  require(!text(status).empty(), "status label is empty");
  require(SendMessageW(endpoint, CB_GETCOUNT, 0, 0) >= 1,
          "endpoint list lacks the dry option");
  std::array<wchar_t, 256> item{};
  SendMessageW(endpoint, CB_GETLBTEXT, 0,
               reinterpret_cast<LPARAM>(item.data()));
  require(std::wstring(item.data()) == L"None - dry microphone",
          "endpoint list lacks the None label");
  ParameterInfo endpointInfo{};
  bool foundEndpoint = false;
  for (int32 i = 0; i < controller->getParameterCount(); ++i) {
    ParameterInfo info{};
    require(controller->getParameterInfo(i, info) == kResultOk,
            "controller parameter enumeration failed");
    if (info.id == endpointId) {
      endpointInfo = info;
      foundEndpoint = true;
    }
  }
  require(foundEndpoint && SendMessageW(endpoint, CB_GETCOUNT, 0, 0) ==
                               endpointInfo.stepCount + 1,
          "editor invented or omitted endpoints");
  for (int32 i = 0; i <= endpointInfo.stepCount; ++i) {
    String128 expected{};
    const double value = endpointInfo.stepCount
                             ? static_cast<double>(i) / endpointInfo.stepCount
                             : 0;
    require(controller->getParamStringByValue(endpointId, value, expected) ==
                kResultOk,
            "endpoint display conversion failed");
    const auto length = SendMessageW(endpoint, CB_GETLBTEXTLEN, i, 0);
    require(length >= 0 && length < 4096, "invalid endpoint text length");
    std::vector<wchar_t> actual(static_cast<size_t>(length) + 1);
    SendMessageW(endpoint, CB_GETLBTEXT, i,
                 reinterpret_cast<LPARAM>(actual.data()));
    std::wstring label;
    for (auto ch : expected) {
      if (!ch)
        break;
      label.push_back(static_cast<wchar_t>(ch));
    }
    require(std::wstring(actual.data()) == label,
            "endpoint text differs from controller");
  }
  std::cout << "Enumerated playback-reference endpoints: "
            << endpointInfo.stepCount << '\n';
  for (HWND control : {enable, endpoint, bypass, status, diagnostics, action,
                       capture, cancelCapture, captureStatus}) {
    RECT client{}, bounds{};
    GetClientRect(GetParent(control), &client);
    GetWindowRect(control, &bounds);
    MapWindowPoints(nullptr, GetParent(control),
                    reinterpret_cast<POINT *>(&bounds), 2);
    require(bounds.left >= 0 && bounds.top >= 0 &&
                bounds.right <= client.right &&
                bounds.bottom <= client.bottom &&
                bounds.right - bounds.left >= 200 &&
                bounds.bottom - bounds.top >= 20,
            "editor control is clipped or too small");
    auto font =
        reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    LOGFONTW fontInfo{};
    require(font && GetObjectW(font, sizeof(fontInfo), &fontInfo) &&
                std::abs(fontInfo.lfHeight) >= 14,
            "editor control has an unreadable font");
  }
  refresh(status);
  require(text(status) == L"Dry - no reference selected",
          "connected processor no-reference status did not reach editor");
  require(text(captureStatus).find(L"Records only after confirmation") !=
                  std::wstring::npos &&
              IsWindowEnabled(capture) && !IsWindowEnabled(cancelCapture),
          "local diagnostic capture must start idle with explicit consent");
  const auto liveDetails = text(diagnostics);
  require(
      liveDetails.find(L"Mic: RMS -9.4 / peak -5.0 dBFS") !=
              std::wstring::npos &&
          liveDetails.find(L"peak ") != std::wstring::npos &&
          liveDetails.find(L"dBFS") != std::wstring::npos &&
          liveDetails.find(L"Reference: unavailable") != std::wstring::npos &&
          liveDetails.find(L"Host: 48000 Hz") != std::wstring::npos &&
          liveDetails.find(L"changed: 0 of 128") != std::wstring::npos &&
          liveDetails.find(L"Gaps:") != std::wstring::npos &&
          liveDetails.find(L"underruns:") != std::wstring::npos &&
          liveDetails.find(L"Peak correlation: unavailable") !=
              std::wstring::npos &&
          liveDetails.find(L"Candidate lag: unavailable") != std::wstring::npos,
      "real processor diagnostics did not reach the editor");
  require(text(action).find(L"select the playback device") !=
              std::wstring::npos,
          "missing reference has no corrective action");
  silentDryBlock(handler.processor);
  refresh(status);
  require(text(diagnostics).find(L"Mic: RMS -inf (silence)") !=
                  std::wstring::npos &&
              text(diagnostics).find(L"Reference: unavailable") !=
                  std::wstring::npos,
          "silent microphone was confused with unavailable reference");
  require(SendMessageW(endpoint, CB_GETCURSEL, 0, 0) == 0,
          "missing reference is not visibly selected as None");
  require(SendMessageW(enable, BM_GETCHECK, 0, 0) == BST_CHECKED,
          "restored enable state is not visible");
  SendMessageW(enable, BM_CLICK, 0, 0);
  handler.expect(enabledId, 0);
  require(controller->getParamNormalized(enabledId) == 0,
          "enable click did not update controller");
  SendMessageW(bypass, BM_CLICK, 0, 0);
  handler.expect(bypassId, 1);
  require(controller->getParamNormalized(bypassId) == 1,
          "bypass click did not update controller");
  SendMessageW(endpoint, CB_SETCURSEL, 0, 0);
  SendMessageW(GetParent(endpoint), WM_COMMAND,
               MAKEWPARAM(endpointControlId, CBN_SELCHANGE),
               reinterpret_cast<LPARAM>(endpoint));
  handler.expect(endpointId, 0);
  const int savedSelection = endpointInfo.stepCount > 0 ? 1 : 0;
  const double savedValue =
      endpointInfo.stepCount > 0 ? 1.0 / endpointInfo.stepCount : 0.0;
  SendMessageW(endpoint, CB_SETCURSEL, savedSelection, 0);
  SendMessageW(GetParent(endpoint), WM_COMMAND,
               MAKEWPARAM(endpointControlId, CBN_SELCHANGE),
               reinterpret_cast<LPARAM>(endpoint));
  handler.expect(endpointId, savedValue);
  MemoryStream saved;
  require(component->getState(&saved) == kResultOk,
          "cannot save selected endpoint while bypassed");
  require(controller->setParamNormalized(endpointId, 0) == kResultOk,
          "cannot clear controller endpoint before restore");
  saved.seek(0, IBStream::kIBSeekSet, nullptr);
  require(controller->setComponentState(&saved) == kResultOk &&
              controller->getParamNormalized(endpointId) == savedValue,
          "saved real endpoint selection did not survive component restore");
  refresh(status);
  require(SendMessageW(endpoint, CB_GETCURSEL, 0, 0) == savedSelection,
          "restored endpoint selection is not visible");

  refresh(status);
  require(text(status) == L"Dry - bypassed",
          "bypass status update did not refresh");
  require(text(action).find(L"turn off bypass") != std::wstring::npos,
          "bypass corrective action did not refresh");
  ViewRect resized(0, 0, 760, 480);
  require(view->canResize() == kResultOk &&
              view->checkSizeConstraint(&resized) == kResultOk &&
              view->onSize(&resized) == kResultOk,
          "editor resize failed");
  require(controllerConnection->disconnect(processorConnection) == kResultOk &&
              processorConnection->disconnect(controllerConnection) ==
                  kResultOk,
          "status disconnect failed");
  refresh(status);
  require(text(status) == L"Status unavailable - waiting for processor",
          "disconnected processor retained a misleading status");
  require(
      text(diagnostics).find(L"Mic: unavailable") != std::wstring::npos &&
          text(diagnostics).find(L"Rates: unavailable") != std::wstring::npos &&
          text(action).find(L"reconnect the processor") != std::wstring::npos,
      "disconnect retained live diagnostics or omitted recovery guidance");
  DiagnosticPeer diagnosticPeer(controllerConnection);
  auto &observation = diagnosticPeer.observation;
  observation.status = buncancel::plugin::ProcessingStatus::readyMonitoring;
  observation.alignment_reason = buncancel::plugin::AlignmentReason::aligned;
  observation.mic_available = observation.reference_available = true;
  observation.dsp_available = observation.aligned = true;
  observation.host_sample_rate = observation.reference_sample_rate = 48000;
  observation.mic_rms = 0.02f;
  observation.output_rms = 0.02f;
  observation.filter_mic_rms = observation.filter_output_rms = 0.02f;
  observation.mic_peak = observation.reference_peak = 0.05f;
  observation.reference_rms = 0.0125f;
  observation.candidate_valid = observation.candidate_confirmed = true;
  observation.correlation = 0.747f;
  observation.candidate_delay_ms = observation.active_delay_ms = 272;
  observation.live_confidence = 0.7f;
  observation.adapted_samples = 8000;
  observation.weights_norm = 1;
  observation.freeze_ratio = 1;
  observation.prediction_rms = 0.001f;
  observation.filter_age_ms = 4000;
  observation.block_samples = 256;
  observation.shadow_decision = 4;
  observation.shadow_validation_count = 12;
  observation.shadow_training_eligibility = 0.9f;
  observation.shadow_validation_eligibility = 0.25f;
  observation.window_mic_observed = observation.window_reference_observed =
      true;
  observation.window_mic_rms = 0.02f;
  observation.window_output_rms = 0.02f;
  observation.window_reference_rms = 0.0125f;
  observation.window_downmix_rms = 0.01f;
  observation.window_mic_seconds = observation.window_reference_seconds = 3;
  observation.reference_receipt_valid = observation.mic_activity_valid =
      observation.reference_activity_valid = true;
  observation.reference_receipt_age_ms = 5;
  observation.mic_activity_age_ms = 100;
  observation.reference_activity_age_ms = 400;
  observation.last_underrun_valid = true;
  observation.last_underrun_age_ms = 1250;
  require(controllerConnection->connect(&diagnosticPeer) == kResultOk,
          "controlled diagnostic peer connection failed");
  refresh(status);
  require(text(status) == L"Ready - aligned / filter monitoring" &&
              text(action).find(L"frozen by near-end protection") !=
                  std::wstring::npos,
          "aligned protected filter did not explain absent subtraction");
  const auto stalled = text(diagnostics);
  for (const auto *expected :
       {L"(mono, last block)", L"changed: 0 of 256", L"Live confidence: 0.700",
        L"Peak correlation: 0.747", L"Active echo lag: 272.00",
        L"Adapted samples: 8000", L"weights L1: 1.0000",
        L"adaptation frozen: 100.0%", L"Filter age: 4.00 s",
        L"last underrun: 1.25 s ago", L"Prediction RMS:",
        L"3 s evidence window", L"Last reference packet received: 0.005 s ago",
        L"mic 0.10 s ago / ref 0.40 s ago", L"Window downmix RMS/peak:",
        L"Last candidate outcome: insufficient correlated evidence",
        L"Validation cycles: 12"})
    require(stalled.find(expected) != std::wstring::npos,
            "detailed stalled-filter observation was omitted");
  require((GetWindowLongW(diagnostics, GWL_STYLE) & ES_READONLY) != 0 &&
              (GetWindowLongW(diagnostics, GWL_STYLE) & WS_VSCROLL) != 0,
          "long diagnostics must remain readable and scrollable");
  SendMessageW(diagnostics, EM_LINESCROLL, 0, 100);
  require(SendMessageW(diagnostics, EM_GETFIRSTVISIBLELINE, 0, 0) > 0,
          "filter details cannot be reached by scrolling");
  observation.subtraction_active = true;
  observation.changed_samples = 256;
  observation.delta_rms = 0.001f;
  observation.filter_output_rms = 0.015f;
  refresh(status);
  require(text(status) == L"Ready - subtracting / reduction unconfirmed" &&
              text(diagnostics).find(L"changed: 256 of 256") !=
                  std::wstring::npos &&
              text(diagnostics).find(L"/ +2.50 dB") != std::wstring::npos &&
              SendMessageW(diagnostics, EM_GETFIRSTVISIBLELINE, 0, 0) > 0,
          "actual subtraction was confused with measured reduction");
  require(diagnosticPeer.captureRequests == 0 &&
              diagnosticPeer.cancelRequests == 0,
          "refresh or host observation triggered local capture");
  respondToConsent(capture, IDNO);
  require(diagnosticPeer.captureRequests == 0,
          "declining capture consent still reached processor");
  respondToConsent(capture, IDYES);
  require(diagnosticPeer.captureRequests == 1 && !IsWindowEnabled(capture) &&
              IsWindowEnabled(cancelCapture) &&
              text(captureStatus).find(L"Recording next 3 seconds locally") !=
                  std::wstring::npos,
          "accepted capture did not show active recording and cancellation");
  SendMessageW(cancelCapture, BM_CLICK, 0, 0);
  require(diagnosticPeer.cancelRequests == 1 && IsWindowEnabled(capture) &&
              !IsWindowEnabled(cancelCapture) &&
              text(captureStatus).find(L"cancelled") != std::wstring::npos,
          "local capture cancel did not reach processor or update controls");
  observation.capture_state = buncancel::plugin::LocalCaptureState::saved;
  refresh(status);
  require(text(captureStatus).find(L"C:\\BunCancelTest\\capture-001") !=
              std::wstring::npos,
          "local capture result folder is inaccessible in editor");
  observation.status = buncancel::plugin::ProcessingStatus::cancelling;
  observation.reducing = true;
  refresh(status);
  require(text(status).find(L"Cancelling") == 0,
          "measured reduction did not reach the UI");
  require(controllerConnection->disconnect(&diagnosticPeer) == kResultOk,
          "controlled diagnostic peer disconnect failed");
  refresh(status);
  require(text(diagnostics).find(L"Filter telemetry: unavailable") !=
              std::wstring::npos,
          "controlled filter observation stayed live after disconnect");
  require(processorConnection->connect(controllerConnection) == kResultOk &&
              controllerConnection->connect(processorConnection) == kResultOk,
          "status reconnect failed");
  refresh(status);
  require(text(status) == L"Dry - bypassed",
          "status did not recover after reconnect");
  require(view->removed() == kResultOk, "editor detach failed");
  view->setFrame(nullptr);
  view->release();
  require(GetWindow(host, GW_CHILD) == nullptr,
          "detach leaked native controls");
  view = controller->createView(ViewType::kEditor);
  require(view && view->setFrame(&frame) == kResultOk &&
              view->attached(host, kPlatformTypeHWND) == kResultOk,
          "editor reopen failed");
  pump();
  require(SendMessageW(findControl(host, enableControlId), BM_GETCHECK, 0, 0) ==
                  BST_UNCHECKED &&
              SendMessageW(findControl(host, bypassControlId), BM_GETCHECK, 0,
                           0) == BST_CHECKED,
          "editor reopen lost controller state");
  require(SendMessageW(findControl(host, endpointControlId), CB_GETCURSEL, 0,
                       0) == savedSelection,
          "editor reopen lost restored endpoint selection");
  require(
      text(findControl(host, diagnosticsControlId)).find(L"Mic: RMS") !=
              std::wstring::npos &&
          text(findControl(host, actionControlId)).find(L"turn off bypass") !=
              std::wstring::npos,
      "editor reopen lost live diagnostics or bypass guidance");
  require(handler.events.empty(), "host updates/reopen emitted user gestures");
  require(view->removed() == kResultOk, "reopened editor detach failed");
  view->setFrame(nullptr);
  view->release();
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: editor-smoke <exact packaged plugin binary>");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(com), "COM initialization failed");
    HMODULE module = LoadLibraryA(argv[1]);
    require(module != nullptr, "LoadLibrary of packaged module failed");
    const auto init =
        reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
    const auto exit =
        reinterpret_cast<bool (*)()>(GetProcAddress(module, "ExitDll"));
    const auto getFactory = reinterpret_cast<IPluginFactory *(*)()>(
        GetProcAddress(module, "GetPluginFactory"));
    require(init && exit && getFactory && init(), "module exports/init failed");
    auto *factory = getFactory();
    require(factory != nullptr, "factory is null");
    IComponent *component = nullptr;
    IEditController *controller = nullptr;
    for (int32 i = 0; i < factory->countClasses(); ++i) {
      PClassInfo info{};
      require(factory->getClassInfo(i, &info) == kResultOk,
              "class query failed");
      if (std::strcmp(info.category, kVstAudioEffectClass) == 0)
        require(factory->createInstance(
                    info.cid, IComponent::iid,
                    reinterpret_cast<void **>(&component)) == kResultOk,
                "processor creation failed");
      if (std::strcmp(info.category, kVstComponentControllerClass) == 0)
        require(factory->createInstance(
                    info.cid, IEditController::iid,
                    reinterpret_cast<void **>(&controller)) == kResultOk,
                "controller creation failed");
    }
    require(component && controller, "module does not expose both classes");
    HostApplication application;
    require(component->initialize(&application) == kResultOk &&
                controller->initialize(&application) == kResultOk,
            "plugin initialize failed");
    IAudioProcessor *processor = nullptr;
    require(component->queryInterface(IAudioProcessor::iid,
                                      reinterpret_cast<void **>(&processor)) ==
                kResultOk,
            "processor interface is missing");
    IConnectionPoint *processorConnection = nullptr,
                     *controllerConnection = nullptr;
    require(component->queryInterface(
                IConnectionPoint::iid,
                reinterpret_cast<void **>(&processorConnection)) == kResultOk &&
                controller->queryInterface(
                    IConnectionPoint::iid,
                    reinterpret_cast<void **>(&controllerConnection)) ==
                    kResultOk,
            "connection points are missing");
    require(processorConnection->connect(controllerConnection) == kResultOk &&
                controllerConnection->connect(processorConnection) == kResultOk,
            "component/controller connection failed");
    Handler handler;
    handler.processor = processor;
    require(controller->setComponentHandler(&handler) == kResultOk,
            "component handler setup failed");
    dryProcessor(component, processor, controller);
    // A captioned top-level window has a system minimum tracking width.
    // Use a borderless host to exercise an actual 32-pixel client rectangle.
    HWND host = CreateWindowExW(0, L"STATIC", L"BunCancel smoke host",
                                WS_POPUP | WS_VISIBLE, 0, 0, 32, 32, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    require(host != nullptr, "host window creation failed");
    editor(component, controller, handler, host, controllerConnection,
           processorConnection);
    DestroyWindow(host);
    const auto stopped = processor->setProcessing(false);
    require((stopped == kResultOk || stopped == kNotImplemented) &&
                component->setActive(false) == kResultOk,
            "processor shutdown failed");
    processorConnection->disconnect(controllerConnection);
    controllerConnection->disconnect(processorConnection);
    processorConnection->release();
    controllerConnection->release();
    controller->setComponentHandler(nullptr);
    require(controller->terminate() == kResultOk &&
                component->terminate() == kResultOk,
            "plugin terminate failed");
    processor->release();
    controller->release();
    component->release();
    factory->release();
    require(exit(), "module shutdown failed");
    FreeLibrary(module);
    CoUninitialize();
    std::cout << "Packaged module: editor, gestures, state, missing-reference "
                 "dry path passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Packaged module smoke failed: " << error.what() << '\n';
    return 1;
  }
}
