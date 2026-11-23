#pragma once

namespace Steinberg {
class IPlugView;
namespace Vst {
class EditController;
}
} // namespace Steinberg

namespace buncancel::plugin {
inline constexpr int enableControlId = 1001;
inline constexpr int endpointControlId = 1002;
inline constexpr int bypassControlId = 1003;
inline constexpr int statusControlId = 1004;

void refreshStatus(Steinberg::Vst::EditController *controller);

Steinberg::IPlugView *createEditor(Steinberg::Vst::EditController *controller);
} // namespace buncancel::plugin
