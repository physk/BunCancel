#include "editor.hpp"
#include "diagnostics.hpp"

#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace buncancel::plugin {
namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr ParamID enabledId = 0;
constexpr ParamID endpointId = 1;
constexpr ParamID bypassId = 2;
constexpr ParamID statusId = 3;
constexpr UINT_PTR refreshTimerId = 1;

UINT windowDpi(HWND window) {
  using GetDpi = UINT(WINAPI *)(HWND);
  const auto getDpi = reinterpret_cast<GetDpi>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  return getDpi ? getDpi(window) : 96;
}

std::wstring displayString(EditController &controller, ParamID id,
                           ParamValue value) {
  String128 text{};
  if (controller.getParamStringByValue(id, value, text) != kResultOk)
    return L"Unavailable";
  std::wstring result;
  for (const auto character : text) {
    if (!character)
      break;
    result.push_back(static_cast<wchar_t>(character));
  }
  return result;
}

std::wstring level(float amplitude) {
  if (!std::isfinite(amplitude) || amplitude < 0.0f)
    return L"unavailable";
  if (amplitude == 0.0f)
    return L"-inf (silence)";
  wchar_t text[48]{};
  std::swprintf(text, std::size(text), L"%.1f", 20.0 * std::log10(amplitude));
  return text;
}

std::wstring meter(const wchar_t *name, bool available, float rms, float peak) {
  if (!available)
    return std::wstring(name) + L": unavailable";
  return std::wstring(name) + L": RMS " + level(rms) + L" / peak " +
         level(peak) + L" dBFS";
}

const wchar_t *nextAction(ProcessingStatus status) {
  switch (status) {
  case ProcessingStatus::dry:
    return L"Next: select a playback reference, then enable experimental AEC.";
  case ProcessingStatus::bypassed:
    return L"Next: turn off bypass to resume experimental AEC.";
  case ProcessingStatus::noReference:
    return L"Next: select the playback device or mix that leaks into the mic.";
  case ProcessingStatus::rateMismatch:
    return L"Next: set the host and playback device to the same sample rate.";
  case ProcessingStatus::unsupportedFormat:
    return L"Next: choose a supported playback format or another reference.";
  case ProcessingStatus::deviceError:
    return L"Next: reconnect the playback device, then reselect the reference.";
  case ProcessingStatus::silentMicrophone:
    return L"No action required. If speech is expected, check the host mic "
           L"input and gain.";
  case ProcessingStatus::silentReference:
    return L"No action required. The reference is connected and currently "
           L"quiet.";
  case ProcessingStatus::interruptedReference:
    return L"Next: check device stability and host buffer size; mic stays dry.";
  case ProcessingStatus::selfMonitoring:
    return L"Next: remove microphone monitoring from the selected playback "
           L"mix.";
  case ProcessingStatus::delayOutOfRange:
    return L"Next: compare timing with a reference closer to the playback "
           L"source.";
  case ProcessingStatus::insufficientCorrelation:
    return L"No action required. Monitoring; no correlated echo detected.";
  case ProcessingStatus::ambiguousAlignment:
    return L"No action required. Monitoring while the echo delay is ambiguous.";
  case ProcessingStatus::awaitingReference:
    return L"Next: check that the selected reference is connected and host "
           L"processing is running.";
  case ProcessingStatus::acquiringAlignment:
    return L"No action required. Monitoring while alignment settles.";
  case ProcessingStatus::aligned:
    return L"Next: check residual echo while speaking; alignment alone does "
           L"not prove reduction.";
  case ProcessingStatus::readyMonitoring:
    return L"No action required. Ready to monitor playback and preserve your "
           L"microphone.";
  case ProcessingStatus::cancelling:
    return L"Next: listen for voice retention during playback; level reduction "
           L"is not ERLE.";
  case ProcessingStatus::unavailable:
  case ProcessingStatus::count:
    return L"Next: start host processing or reconnect the processor to receive "
           L"diagnostics.";
  }
  return L"Next: check the host audio connection.";
}

std::wstring diagnosticsText(const Diagnostics &value, bool available) {
  std::wstring result = meter(L"Mic", available && value.mic_available,
                              value.mic_rms, value.mic_peak);
  result +=
      L"\r\n" + meter(L"Reference", available && value.reference_available,
                      value.reference_rms, value.reference_peak);
  if (!available)
    return result +
           L"\r\nRates: unavailable\r\nGaps / underruns: unavailable"
           L"\r\nPeak correlation: unavailable"
           L"\r\nCandidate lag: unavailable\r\nPacket timing: unavailable";
  wchar_t text[256]{};
  if (value.host_sample_rate > 0.0)
    std::swprintf(text, std::size(text), L"\r\nHost: %.0f Hz / capture: ",
                  value.host_sample_rate);
  else
    std::swprintf(text, std::size(text), L"\r\nHost: unavailable / capture: ");
  result += text;
  result += value.reference_sample_rate
                ? std::to_wstring(value.reference_sample_rate) + L" Hz"
                : L"unavailable";
  std::swprintf(text, std::size(text), L"\r\nGaps: %llu / underruns: %llu",
                static_cast<unsigned long long>(value.dropouts),
                static_cast<unsigned long long>(value.underruns));
  result += text;
  if (value.candidate_valid) {
    std::swprintf(text, std::size(text),
                  L"\r\nPeak correlation: %.3f\r\nCandidate lag: %.2f ms%ls",
                  value.correlation, value.candidate_delay_ms,
                  value.aligned               ? L" (aligned)"
                  : value.candidate_confirmed ? L" (confirmed)"
                                              : L" (unconfirmed)");
    result += text;
  } else {
    result +=
        L"\r\nPeak correlation: unavailable\r\nCandidate lag: unavailable";
  }
  if (value.timing_valid) {
    std::swprintf(
        text, std::size(text),
        L"\r\nMic start minus latest packet: %+.2f ms / holdback: %.0f ms",
        value.timestamp_offset_ms, value.reference_holdback_ms);
    result += text;
  } else {
    result += L"\r\nPacket timing: unavailable";
  }
  return result;
}

class NativeEditor final : public CPluginView {
public:
  explicit NativeEditor(EditController *controller) : controller_(controller) {
    rect = ViewRect(0, 0, 660, 600);
  }

  ~NativeEditor() override { detachWindow(); }

  tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
    return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue
                                                             : kResultFalse;
  }

  tresult PLUGIN_API attached(void *parent, FIDString type) override {
    IPtr<NativeEditor> retained(this);
    if (isPlatformTypeSupported(type) != kResultTrue || !parent || window_ ||
        !IsWindow(static_cast<HWND>(parent)))
      return kResultFalse;

    if (classRegistered_)
      detachWindow();

    // Each view owns its class, including across concurrent editor instances.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&windowProcedure),
                            &module_))
      return kResultFalse;
    std::swprintf(className_, std::size(className_), L"BunCancel.Editor.%p",
                  static_cast<void *>(this));
    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.hInstance = module_;
    definition.lpfnWndProc = windowProcedure;
    definition.lpszClassName = className_;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClassExW(&definition))
      return kResultFalse;
    classRegistered_ = true;
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, className_, L"BunCancel",
                              WS_CHILD | WS_CLIPCHILDREN, 0, 0, rect.getWidth(),
                              rect.getHeight(), static_cast<HWND>(parent),
                              nullptr, module_, this);
    if (!window_ || !createControls()) {
      detachWindow();
      return kResultFalse;
    }
    systemWindow = parent;
    dpi_ = windowDpi(window_);
    layout();
    refreshStatus(controller_);
    if (!window_)
      return kResultFalse;
    refresh();
    if (!SetTimer(window_, refreshTimerId, 100, nullptr)) {
      detachWindow();
      return kResultFalse;
    }
    ShowWindow(window_, SW_SHOW);
    requestDpiSize(true);
    return window_ ? kResultOk : kResultFalse;
  }

  tresult PLUGIN_API removed() override {
    detachWindow();
    return kResultOk;
  }

  tresult PLUGIN_API onSize(ViewRect *size) override {
    if (!size || size->getWidth() <= 0 || size->getHeight() <= 0)
      return kInvalidArgument;
    rect = *size;
    if (window_) {
      SetWindowPos(window_, nullptr, 0, 0, rect.getWidth(), rect.getHeight(),
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
      layout();
    }
    return kResultOk;
  }

  tresult PLUGIN_API canResize() override { return kResultTrue; }

  tresult PLUGIN_API checkSizeConstraint(ViewRect *size) override {
    if (!size)
      return kInvalidArgument;
    size->right =
        size->left + std::max(size->getWidth(), MulDiv(560, dpi_, 96));
    size->bottom =
        size->top + std::max(size->getHeight(), MulDiv(600, dpi_, 96));
    return kResultOk;
  }

  tresult PLUGIN_API onFocus(TBool state) override {
    if (state && enable_)
      SetFocus(enable_);
    return kResultOk;
  }

private:
  HWND control(const wchar_t *type, const wchar_t *label, DWORD style, int id) {
    return CreateWindowExW(
        0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module_, nullptr);
  }

  bool createControls() {
    title_ = control(L"STATIC", L"BunCancel", SS_LEFT, 0);
    subtitle_ = control(L"STATIC", L"Playback echo cancellation", SS_LEFT, 0);
    enable_ = control(L"BUTTON", L"Enable experimental AEC",
                      BS_AUTOCHECKBOX | WS_TABSTOP, enableControlId);
    sourceLabel_ = control(L"STATIC", L"Playback reference", SS_LEFT, 0);
    endpoint_ =
        control(L"COMBOBOX", L"",
                CBS_DROPDOWNLIST | WS_VSCROLL | CBS_AUTOHSCROLL | WS_TABSTOP,
                endpointControlId);
    bypass_ = control(L"BUTTON", L"Bypass (dry microphone)",
                      BS_AUTOCHECKBOX | WS_TABSTOP, bypassControlId);
    statusLabel_ = control(L"STATIC", L"Reference / AEC status", SS_LEFT, 0);
    status_ = control(L"STATIC", L"", SS_LEFT, statusControlId);
    diagnostics_ = control(L"STATIC", L"", SS_LEFT, diagnosticsControlId);
    action_ = control(L"STATIC", L"", SS_LEFT, actionControlId);
    note_ =
        control(L"STATIC",
                L"Use a playback mix without microphone monitoring.\r\n"
                L"Missing reference keeps the microphone dry.\r\n"
                L"Timing = estimated mic start - reference packet start (QPC).",
                SS_LEFT, 0);
    if (!title_ || !subtitle_ || !enable_ || !sourceLabel_ || !endpoint_ ||
        !bypass_ || !statusLabel_ || !status_ || !diagnostics_ || !action_ ||
        !note_)
      return false;
    ParameterInfo info{};
    if (controller_->getParameterInfoByTag(endpointId, info) != kResultOk ||
        info.stepCount < 0)
      return false;
    endpointSteps_ = info.stepCount;
    for (int32 index = 0; index <= endpointSteps_; ++index) {
      const auto value =
          endpointSteps_ ? static_cast<double>(index) / endpointSteps_ : 0.0;
      const auto label = displayString(*controller_, endpointId, value);
      if (SendMessageW(endpoint_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(label.c_str())) < 0)
        return false;
    }
    return true;
  }

  void layout() {
    if (!window_)
      return;
    RECT bounds{};
    GetClientRect(window_, &bounds);
    // A host may decline a DPI resize; keep every control visible in that case.
    const auto scale =
        std::min({dpi_ / 96.0, bounds.right / 560.0, bounds.bottom / 600.0});
    const auto pixel = [scale](int value) {
      return std::max(1, static_cast<int>(std::lround(value * scale)));
    };
    auto font = CreateFontW(-pixel(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (font) {
      for (auto child :
           {title_, subtitle_, enable_, sourceLabel_, endpoint_, bypass_,
            statusLabel_, status_, diagnostics_, action_, note_})
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      if (font_)
        DeleteObject(font_);
      font_ = font;
    }
    const int width = std::max(1, static_cast<int>(bounds.right) - pixel(48));
    const auto place = [&](HWND child, int top, int height) {
      MoveWindow(child, pixel(24), pixel(top), width, pixel(height), TRUE);
    };
    place(title_, 20, 26);
    place(subtitle_, 50, 24);
    place(enable_, 88, 28);
    place(sourceLabel_, 132, 24);
    place(endpoint_, 158, 240);
    place(bypass_, 204, 28);
    place(statusLabel_, 250, 22);
    place(status_, 276, 28);
    place(diagnostics_, 312, 150);
    place(action_, 470, 56);
    place(note_, 532, 60);
  }

  void refresh() {
    SendMessageW(enable_, BM_SETCHECK,
                 controller_->getParamNormalized(enabledId) >= 0.5
                     ? BST_CHECKED
                     : BST_UNCHECKED,
                 0);
    SendMessageW(bypass_, BM_SETCHECK,
                 controller_->getParamNormalized(bypassId) >= 0.5
                     ? BST_CHECKED
                     : BST_UNCHECKED,
                 0);
    // Do not interrupt a user navigating the open dropdown during automation.
    if (!SendMessageW(endpoint_, CB_GETDROPPEDSTATE, 0, 0)) {
      const auto value =
          std::clamp(controller_->getParamNormalized(endpointId), 0.0, 1.0);
      SendMessageW(endpoint_, CB_SETCURSEL,
                   static_cast<WPARAM>(std::lround(value * endpointSteps_)), 0);
    }
    Diagnostics snapshot;
    const bool available = readDiagnostics(controller_, snapshot);
    auto text = displayString(*controller_, statusId,
                              controller_->getParamNormalized(statusId));
    if (available && snapshot.status == ProcessingStatus::readyMonitoring) {
      if (snapshot.mic_available && snapshot.mic_peak == 0.0f)
        text = L"Ready - microphone quiet";
      else if (snapshot.reference_available && snapshot.reference_peak == 0.0f)
        text = L"Ready - reference quiet";
      else if (snapshot.alignment_reason ==
               AlignmentReason::insufficientCorrelation)
        text = L"Ready - monitoring / no correlated echo";
      else {
        switch (snapshot.alignment_reason) {
        case AlignmentReason::selfMonitoring:
          text = L"Ready - monitoring / possible mic in reference";
          break;
        case AlignmentReason::delayOutOfRange:
          text = L"Ready - monitoring / echo delay outside range";
          break;
        case AlignmentReason::ambiguous:
          text = L"Ready - monitoring / ambiguous echo delay";
          break;
        case AlignmentReason::aligned:
          text = L"Ready - aligned / no subtraction observed";
          break;
        default:
          text = L"Ready - monitoring";
          break;
        }
      }
    }
    if (text != statusText_) {
      statusText_ = std::move(text);
      SetWindowTextW(status_, statusText_.c_str());
    }
    const auto details = diagnosticsText(snapshot, available);
    if (details != diagnosticsText_) {
      diagnosticsText_ = details;
      SetWindowTextW(diagnostics_, details.c_str());
    }
    auto actionStatus =
        available ? snapshot.status : ProcessingStatus::unavailable;
    if (available && snapshot.status == ProcessingStatus::readyMonitoring) {
      if (snapshot.mic_available && snapshot.mic_peak == 0.0f)
        actionStatus = ProcessingStatus::silentMicrophone;
      else if (snapshot.reference_available && snapshot.reference_peak == 0.0f)
        actionStatus = ProcessingStatus::silentReference;
      else if (snapshot.alignment_reason == AlignmentReason::selfMonitoring)
        actionStatus = ProcessingStatus::selfMonitoring;
      else if (snapshot.alignment_reason == AlignmentReason::delayOutOfRange)
        actionStatus = ProcessingStatus::delayOutOfRange;
    }
    SetWindowTextW(action_, nextAction(actionStatus));
  }

  void edit(ParamID id, ParamValue value) {
    if (controller_->beginEdit(id) != kResultOk)
      return;
    controller_->setParamNormalized(id, value);
    controller_->performEdit(id, value);
    controller_->endEdit(id);
    if (window_)
      refresh();
  }

  void requestDpiSize(bool force = false) {
    if (!plugFrame)
      return;
    ViewRect desired(0, 0, MulDiv(660, dpi_, 96), MulDiv(600, dpi_, 96));
    if (force || desired.getWidth() != rect.getWidth() ||
        desired.getHeight() != rect.getHeight())
      plugFrame->resizeView(this, &desired);
  }

  void detachWindow() {
    if (window_)
      DestroyWindow(window_);
    window_ = nullptr;
    systemWindow = nullptr;
    title_ = subtitle_ = enable_ = sourceLabel_ = endpoint_ = bypass_ =
        statusLabel_ = status_ = diagnostics_ = action_ = note_ = nullptr;
    if (font_) {
      DeleteObject(font_);
      font_ = nullptr;
    }
    if (classRegistered_) {
      UnregisterClassW(className_, module_);
      classRegistered_ = false;
    }
    statusText_.clear();
    diagnosticsText_.clear();
  }

  static LRESULT CALLBACK windowProcedure(HWND window, UINT message,
                                          WPARAM wParam, LPARAM lParam) {
    auto *self = reinterpret_cast<NativeEditor *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<NativeEditor *>(
          reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
      return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_TIMER:
      if (wParam == refreshTimerId) {
        self->addRef();
        refreshStatus(self->controller_);
        if (self->window_)
          self->refresh();
        self->release();
      }
      return 0;
    case WM_COMMAND: {
      // Host edit callbacks can synchronously close and release the view.
      self->addRef();
      const auto id = LOWORD(wParam);
      const auto notification = HIWORD(wParam);
      if (id == enableControlId && notification == BN_CLICKED)
        self->edit(enabledId,
                   SendMessageW(self->enable_, BM_GETCHECK, 0, 0) == BST_CHECKED
                       ? 1.0
                       : 0.0);
      else if (id == bypassControlId && notification == BN_CLICKED)
        self->edit(bypassId,
                   SendMessageW(self->bypass_, BM_GETCHECK, 0, 0) == BST_CHECKED
                       ? 1.0
                       : 0.0);
      else if (id == endpointControlId && notification == CBN_SELCHANGE) {
        const auto selected = SendMessageW(self->endpoint_, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected <= self->endpointSteps_)
          self->edit(endpointId,
                     self->endpointSteps_
                         ? static_cast<double>(selected) / self->endpointSteps_
                         : 0.0);
      }
      self->release();
      return 0;
    }
    case WM_DPICHANGED:
    case WM_DPICHANGED_AFTERPARENT:
      self->dpi_ = windowDpi(window);
      self->layout();
      self->requestDpiSize();
      return 0;
    case WM_NCDESTROY:
      KillTimer(window, refreshTimerId);
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      self->window_ = nullptr;
      self->systemWindow = nullptr;
      break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
  }

  IPtr<EditController> controller_;
  HWND window_ = nullptr;
  HWND title_ = nullptr, subtitle_ = nullptr, enable_ = nullptr;
  HWND sourceLabel_ = nullptr, endpoint_ = nullptr, bypass_ = nullptr;
  HWND statusLabel_ = nullptr, status_ = nullptr, note_ = nullptr;
  HWND diagnostics_ = nullptr, action_ = nullptr;
  HINSTANCE module_ = nullptr;
  HFONT font_ = nullptr;
  wchar_t className_[80]{};
  bool classRegistered_ = false;
  UINT dpi_ = 96;
  int32 endpointSteps_ = 0;
  std::wstring statusText_;
  std::wstring diagnosticsText_;
};
} // namespace

Steinberg::IPlugView *createEditor(Steinberg::Vst::EditController *controller) {
  return controller ? new NativeEditor(controller) : nullptr;
}
} // namespace buncancel::plugin
#else
namespace buncancel::plugin {
Steinberg::IPlugView *createEditor(Steinberg::Vst::EditController *) {
  return nullptr;
}
} // namespace buncancel::plugin
#endif
