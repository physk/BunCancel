# Proposed technical architecture

**Status:** hypothesis and preferred boundaries, not a proven implementation.

## Signal flow

1. **Reference worker (Windows):** discovers a selected endpoint; captures via WASAPI render loopback when the endpoint supports it, or opens an exposed capture endpoint when suitable. It timestamps buffers and reports discontinuities.
2. **Clock and format layer:** converts reference format/sample rate, tracks drift and aligns with the microphone time base. A bounded queue transports samples to the DSP without audio-thread blocking.
3. **Mic input / VST3 callback:** consumes input blocks and aligned reference blocks; calls a real-time-safe DSP engine; outputs only the cleaned microphone.
4. **AEC DSP core:** delay estimation, adaptive acoustic path, near-end activity/double-talk detector, residual echo estimator and optional constrained postfilter.
5. **Control / UI:** endpoint picker, health/status, meters, diagnostics and saved settings; never directly accesses the real-time callback state unsafely.
6. **Offline runner:** calls the very same DSP core with WAV fixtures, no live devices or VST host required.

## Candidate module layout

- src/dsp/ — portable C++20 DSP, math utilities, adaptive filters and detectors.
- src/capture/windows/ — WASAPI endpoint discovery, capture and timing, async worker lifecycle.
- src/plugin/ — VST3 factory, processing, parameters and host lifecycle.
- src/ui/ — independent controls and presentation.
- tests/audio/ — fixture generation, deterministic corpora and validation.
- tests/dsp/ — synthetic/real audio evaluation, CPU benchmarks and callback constraints.
- tools/ — offline runner, reporting and packaging.

These directories are a target architecture and may be introduced incrementally; no empty placeholders are required.

## Research targets

- Delay estimation with robust coarse search, sub-frame refinement and confidence score.
- Frequency-domain partitioned block adaptive filter with finite tails and bounded adaptation, potentially using NLMS as a reference baseline.
- Double-talk-aware freeze/slow adaptation and near-end preservation.
- Residual echo suppression with careful transition smoothing, not hard spectral muting.
- Stereo render awareness; separate device drift handling via bounded resampling and timestamp reconciliation.
- RT-safe instrumentation: atomics/counters and preallocated telemetry snapshots only.

The goal is **an independent implementation** developed from first principles and scientific literature, not lifted from a third-party AEC codebase. Compare WebRTC AEC3 offline as an external competitor only if licensing and evaluation procedure permit.

## Wave Link caveat

A visible Personal Mix is a Wave Link routing abstraction, **not proof that Windows exposes it in a specific WASAPI endpoint class**. Discover what Wave Link 3 actually exposes at runtime on target systems. If the selected output mix cannot be captured, consider user-configurable endpoint routing, a companion capture service or supported device APIs; do not claim support until the signal path is proven.

## Avoid easy mistakes

- Subtracting numerically equal samples without room-path/delay alignment does not cancel sound from the mic.
- A reference containing microphone monitoring may trigger self-cancellation.
- Volume/gain changes cannot be assumed linear under clipping or loudspeaker distortion.
- Background device capture in the plugin process may face host lifecycle/permission constraints.
- Echo reduction and preserved voice quality are competing constraints and must be reported separately.
- Playback and capture clocks can run at slightly different rates, not merely different start offsets.
- If effective processing needs lookahead, explicitly measure and report introduced latency; do not conceal buffering in a helper service.
