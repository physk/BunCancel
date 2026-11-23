# Roadmap and agent work order

## M0 — Requirements and reproducible fixture lab
- [x] Public project specification and agent guidance
- [x] Original logo asset and license
- [x] Synthetic WAV generator and standard-library fixture smoke tests
- [ ] Independently verified licensed real speech corpus and path IRs
- [ ] Offline scoring tool with rigorously defined temporal alignment and metrics
- [ ] Held-out evaluation split, reproducible A/B artifact report

**Exit:** seed-stable WAVs, manifest and metrics, CI that does not confuse fixture tests with AEC performance.

## M1 — Engine skeleton and baseline
- [ ] Portable C++20 DSP library with sample format/host-neutral I/O
- [ ] Deterministic offline processing CLI for existing WAV fixtures
- [ ] Unit-tested passthrough, delay estimator, finite-tap baseline adaptive filter
- [ ] NaN/denormal/clipping protection and performance instrumentation

**Exit:** measurable initial echo attenuation on the baseline case with separately reported voice distortion.

## M2 — Robust AEC
- [ ] Frequency-domain, partitioned adaptive filtering or justified equivalent
- [ ] Double-talk-aware adaptation
- [ ] Residual echo suppressor
- [ ] Stereo render reference handling
- [ ] Drift compensation and path/volume change recovery
- [ ] Blind held-out evaluations and external baseline comparison

**Exit:** documented quality trade-offs over the full matrix, no near-end disappearance.

## M3 — Windows capture and VST3
- [ ] Confirm Wave Link 3 endpoint topology with actual device enumeration
- [ ] WASAPI worker with timestamped bounded queue, reconnection and device picker
- [ ] Native VST3 integration; latency communication and saved settings
- [ ] Host loading, buffer, bypass and device-failure torture tests
- [ ] Audio-thread allocation/blocking checks

**Exit:** usable experimental plugin with measured processing and host/device stability.

## M4 — Experience and quality
- [ ] Bunny-themed UI, diagnostics and safe controls
- [ ] Optional offline calibration / reference health wizard
- [ ] Windows audio hardware tests, including wireless headphone close-coupling
- [ ] Before/after WAVs and benchmark report using valid recording consent
- [ ] Package verification and public getting-started guide

**Exit:** evidence-based pre-release with known limitations.

## M5 — Release and further platforms
- [ ] Release-grade CI, signed/unsigned installation policy and license checks
- [ ] Reproducible VST3 artifacts and checksums
- [ ] Regression/noise/long-run soak tests
- [ ] Multi-reference and standalone application exploration

**Rule:** no versions marketed as functional AEC before M2 and M3 gates are demonstrated.
