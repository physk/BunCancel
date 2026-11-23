// A minimal Windows host for the exact packaged module supplied on the command
// line.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "base/source/fstreamer.h"
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

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

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
  std::array<wchar_t, 1024> buffer{};
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

  MemoryStream saved;
  require(component->getState(&saved) == kResultOk, "state save failed");
  saved.seek(0, IBStream::kIBSeekSet, nullptr);
  require(controller->setComponentState(&saved) == kResultOk &&
              controller->getParamNormalized(enabledId) == 1 &&
              controller->getParamNormalized(bypassId) == 0 &&
              controller->getParamNormalized(endpointId) == 0,
          "processor state did not preserve dry configuration");
}

void editor(IEditController *controller, Handler &handler, HWND host,
            IConnectionPoint *controllerConnection,
            IConnectionPoint *processorConnection) {
  auto *view = controller->createView(ViewType::kEditor);
  require(view != nullptr, "createView(editor) returned null");
  require(view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultOk,
          "editor does not support HWND");
  ViewRect size{};
  require(view->getSize(&size) == kResultOk && size.getWidth() >= 560 &&
              size.getHeight() >= 380,
          "editor returned invalid initial size");
  require(view->attached(host, kPlatformTypeHWND) == kResultOk,
          "editor attach failed");
  pump();
  const HWND enable = findControl(host, enableControlId);
  const HWND endpoint = findControl(host, endpointControlId);
  const HWND bypass = findControl(host, bypassControlId);
  const HWND status = findControl(host, statusControlId);
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
  for (HWND control : {enable, endpoint, bypass, status}) {
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

  refresh(status);
  require(text(status) == L"Dry - AEC off or bypassed",
          "status update did not refresh");
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
  require(text(status) == L"Status unavailable - processor disconnected",
          "disconnected processor retained a misleading status");
  require(processorConnection->connect(controllerConnection) == kResultOk &&
              controllerConnection->connect(processorConnection) == kResultOk,
          "status reconnect failed");
  refresh(status);
  require(text(status) == L"Dry - AEC off or bypassed",
          "status did not recover after reconnect");
  require(view->removed() == kResultOk, "editor detach failed");
  view->release();
  require(GetWindow(host, GW_CHILD) == nullptr,
          "detach leaked native controls");
  view = controller->createView(ViewType::kEditor);
  require(view && view->attached(host, kPlatformTypeHWND) == kResultOk,
          "editor reopen failed");
  pump();
  require(SendMessageW(findControl(host, enableControlId), BM_GETCHECK, 0, 0) ==
                  BST_UNCHECKED &&
              SendMessageW(findControl(host, bypassControlId), BM_GETCHECK, 0,
                           0) == BST_CHECKED,
          "editor reopen lost controller state");
  require(handler.events.empty(), "host updates/reopen emitted user gestures");
  require(view->removed() == kResultOk, "reopened editor detach failed");
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
    HWND host = CreateWindowExW(
        0, L"STATIC", L"BunCancel smoke host", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        0, 0, 900, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(host != nullptr, "host window creation failed");
    editor(controller, handler, host, controllerConnection,
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
