#pragma once

#include <cstdint>

namespace Steinberg::Vst {
class EditController;
}

namespace buncancel::plugin {

// Values 0..9 retain the original read-only host parameter's ordering.
enum class ProcessingStatus : std::uint32_t {
  dry,
  awaitingReference,
  rateMismatch,
  unsupportedFormat,
  deviceError,
  acquiringAlignment,
  cancelling,
  silentMicrophone,
  noReference,
  unavailable,
  bypassed,
  silentReference,
  interruptedReference,
  insufficientCorrelation,
  ambiguousAlignment,
  selfMonitoring,
  delayOutOfRange,
  aligned,
  readyMonitoring,
  count
};

enum class AlignmentReason : std::uint32_t {
  warmup,
  searching,
  insufficientCorrelation,
  ambiguous,
  selfMonitoring,
  delayOutOfRange,
  aligned,
  referenceMissing
};

struct ProcessingObservation {
  bool enabled{};
  bool bypassed{};
  bool selected{};
  bool connected{};
  bool processing{};
  bool format_supported{true};
  bool rate_matched{true};
  bool device_error{};
  bool interrupted{};
  bool reducing{};
};

// Readiness means the selected stream is compatible and connected. Silence,
// correlation search and near-end-only speech do not make a stream unready.
inline ProcessingStatus
selectProcessingStatus(const ProcessingObservation &value) noexcept {
  if (value.bypassed)
    return ProcessingStatus::bypassed;
  if (!value.enabled)
    return ProcessingStatus::dry;
  if (!value.selected)
    return ProcessingStatus::noReference;
  if (!value.format_supported)
    return ProcessingStatus::unsupportedFormat;
  if (!value.rate_matched)
    return ProcessingStatus::rateMismatch;
  if (value.device_error)
    return ProcessingStatus::deviceError;
  if (!value.connected || !value.processing)
    return ProcessingStatus::awaitingReference;
  if (value.interrupted)
    return ProcessingStatus::interruptedReference;
  return value.reducing ? ProcessingStatus::cancelling
                        : ProcessingStatus::readyMonitoring;
}

// One observation from one processor callback. Reduction is an observed level
// change, never an ERLE measurement or proof of near-end voice preservation.
struct Diagnostics {
  ProcessingStatus status{ProcessingStatus::unavailable};
  std::uint64_t observation_qpc_100ns{};
  AlignmentReason alignment_reason{AlignmentReason::referenceMissing};
  float mic_rms{};
  float mic_peak{};
  float reference_rms{};
  float reference_peak{};
  double host_sample_rate{};
  std::uint32_t reference_sample_rate{};
  std::uint64_t underruns{};
  std::uint64_t dropouts{};
  double timestamp_offset_ms{};
  double reference_holdback_ms{};
  double candidate_delay_ms{};
  double active_delay_ms{};
  float correlation{};
  float reduction_db{};
  float output_rms{};
  float delta_rms{};
  float block_power_delta_db{};
  std::uint32_t changed_samples{};
  std::uint32_t block_samples{};
  double last_underrun_age_ms{};
  std::uint64_t adapted_samples{};
  std::uint64_t shadow_adapted_samples{};
  std::uint64_t shadow_promotions{};
  std::uint64_t filter_resets{};
  double filter_age_ms{};
  float weights_norm{};
  float prediction_rms{};
  float subtraction_rms{};
  float subtraction_gain{};
  float freeze_ratio{};
  float live_confidence{};
  float prediction_coherence{};
  float competing_correlation{};
  float filter_mic_rms{};
  float filter_output_rms{};
  bool shadow_validating{};
  bool dsp_available{};
  bool block_power_delta_valid{};
  bool last_underrun_valid{};
  bool subtraction_active{};
  bool mic_available{};
  bool reference_available{};
  bool timing_valid{};
  bool candidate_valid{};
  bool candidate_confirmed{};
  bool aligned{};
  bool reducing{};
  bool bypassed{};
};

// UI thread only. False means the connection or observation expired.
bool readDiagnostics(Steinberg::Vst::EditController *controller,
                     Diagnostics &snapshot);

} // namespace buncancel::plugin
