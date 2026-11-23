# Product and engineering requirements

Requirements are marked MUST, SHOULD, or LATER. They are design requirements unless specifically marked **verified**.

## R1 — Signals and routing (MUST)

- Process one microphone input stream to one microphone output stream without adding the reference to output.
- Use a user-selectable **far-end playback reference**. Wave Link 3 Personal Mix is an important example, not an assumed universal Windows endpoint name.
- Discover and validate whether selected mixes are presented as WASAPI render-loopback endpoints, capture endpoints or neither. **Do not assume arbitrary virtual capture devices support render loopback.**
- Support a single full headphone/personal mix as the simplest initial reference. Stereo reference should be preserved or explicitly mixed with tested weights, not silently discarded.
- Detect capture loops or a reference substantially contaminated with the near-end microphone and warn; avoid harmful adaptation.
- Gracefully handle devices at different sample rates, host buffer sizes and reconfiguration.

## R2 — Cancellation (MUST)

- Estimate relative delay and track slow timing drift between playback reference and microphone capture.
- Model a finite acoustic path (gain, spectral coloration and reflections), including nonstationary paths.
- Implement original adaptive echo estimation, near-end/double-talk safeguards and bounded residual echo suppression.
- A valid reference stream makes an enabled processor ready to monitor. Playback silence, a muted mic and independent near-end speech are normal operating conditions. AEC must start and pass microphone samples without acoustic calibration or audible leakage.
- Show current subtraction activity separately from capture readiness. Only indicate cancellation when aligned subtraction is reducing measured signal energy. Distinguish measured silence from unavailable samples.
- Preserve near-end speech; never optimize only for output silence.
- Pass through the mic when a reference is missing, interrupted or unusable. Show the connection or processing fault when one is observed. A connected silent reference remains ready.
- Protect against extreme reference level, clipping and nonlinear loudspeaker/headphone distortion. Document the limits honestly.
- Do not assert exact or complete cancellation in any acoustic setup.

## R3 — Plugin/host (MUST)

- Provide native Windows x64 **VST3** suitable for common VST3 hosts; test Wave Link 3 as an initial host.
- Use a supported, reviewed VST3 SDK/toolchain with repeatable dependencies. DSP should be independent of the UI and host.
- Run capture outside the real-time audio callback, with bounded queues, timestamps, explicit overflow recovery and clean device shutdown.
- No audio-callback allocations, blocking, locks, Windows device enumeration, disk I/O or synchronous logging.
- Communicate and measure added latency, buffer behavior and resampling delay. Host settings must survive plugin reload.
- Fail safe if capture permissions/device topology change. Do not automatically overwrite other mixer routes.

## R4 — User experience (SHOULD before 1.0)

- Source selection, start/stop, alignment status and reference meter.
- Dry/reference/processed monitoring mode for controlled troubleshooting, with safe monitoring warnings.
- Controls for aggressiveness only when backed by meaningful DSP behavior.
- An echo attenuation estimate accompanied by confidence and a clear distinction between single-talk and double-talk.
- Minimal friendly interface using the BunCancel bunny identity; readable dark/light contexts and accessible controls.
- Optional standalone companion UI when host restrictions justify a service.

## R5 — Automated test lab (MUST before DSP)

- Generate deterministic WAV fixtures with clean voice, reference, mic mixture and separately known echo.
- Include normal acoustic path, extreme close-coupling, long delay, double-talk, abrupt volume change, silence, path changes, clipping and clock drift.
- Verify sample rates, synchronization, provenance and ground truth integrity.
- Report objective echo and near-end quality metrics with conditions, durations and methodology.
- Run smoke checks in CI, and attach meaningful regressions and audible artifacts to DSP reviews.
- Maintain an untouched evaluation split, plus licensed real-speech and measured-path datasets before calling results representative.

## R6 — Privacy, legal and maintenance (MUST)

- Keep signal processing and diagnostics local. Default operation retains only bounded processing history and aggregate meter readings. Saving audio requires explicit consent for each short diagnostic capture. Never automate recording through a saved setting or upload recordings.
- All media assets must be created in the project or carry a redistributable license and attribution.
- Original AEC code only; do not copy or relicense another AEC engine as original work.
- Third-party dependencies and SDK licenses require per-version review.
- No personal-name or organization-based requirements in public docs, fixtures, UI metadata or comments.

## Acceptance gates

**Gate A: Fixture infrastructure.** Re-running with the same seed/parameters yields byte-identical assets and all three signal roles have verified timing and channel conventions. CI passes without pretending this measures AEC quality.

**Gate B: Offline AEC.** Run recorded reference+mic pairs through a deterministic offline engine. Show properly defined echo metrics during far-end-only intervals, and voice preservation metrics during near-end-only and double-talk intervals. Compare against dry mic and a separately obtained external baseline.

**Gate C: VST3 integration.** Verify host load/unload, buffer sizes, routing, no microphone self-cancellation, known latency, device disconnect, hot reconfigure and no callback underruns.

**Gate D: Hardware acceptance.** Test an actual Wave Link 3 Personal Mix/reference endpoint where exposed, plus a standard Windows playback device. Test wired and wireless paths. Include the headphones-on-mic torture case, recordings, metrics and honest limitations.

**Gate E: Public release.** Automated checks, documentation, supported Windows versions, signed or clearly unsigned package policy, third-party notices and reproducible build instructions. No fabricated benchmark or release badge.
