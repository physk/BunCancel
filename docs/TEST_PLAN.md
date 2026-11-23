# Automated audio test lab

This comes **before** development of the cancellation engine. The ambition is unattended, reproducible progress — not repeated human trial-and-error in a live Discord call.

## File contract

For each scenario, generate:
- **reference.wav** — clean far-end playback reference (stereo, sample-aligned source time axis).
- **clean_voice.wav** — isolated desired near-end microphone voice (mono).
- **echo_only.wav** — acoustic contamination arriving at the microphone (mono).
- **mic_input.wav** — clean_voice + echo_only (+ noise, as modeled), normalized/clipped where defined (mono).
- **manifest.json** — scenario configuration, sample rate, delay, seed, processing operations, clipping conditions, signal descriptions and evaluation boundaries.

All signals have an explicit sample rate, duration, timebase and ownership. Echo_only is the generated pre-mixture contamination; for nonlinear scenarios the sum may differ from the final clipped microphone input.

A future **processed.wav** is produced by the actual DSP under test, never forged by the fixture generator.

## Scenarios

| ID | Stimulus | Expected challenge |
| --- | --- | --- |
| baseline | speech-like far end, short multi-path acoustic echo, quiet near end | initial calibration and cancellation |
| headphones-on-mic | loud, strongly colored short-path playback and close mic | strong leakage; nonlinear distortion exposure |
| double-talk | near-end speaking simultaneously with far-end | protect voice and avoid filter divergence |
| long-delay | playback-to-mic delay plus acoustic reflections | robust delay estimator and latency accounting |
| gain-change | abrupt reference acoustic gain change midway | reconvergence without pumping |
| reference-silent | near-end speech, no far-end source | transparent passthrough |
| nonlinear-clip | clipping on mic input after mixture | identify irreducible nonlinear residue |
| drift | increasing playback-to-mic timing offset | track ppm-level rate mismatch without artifacts |

The initial generator uses synthetic modulated harmonics, deterministic noise and modeled echo paths. **It is not real speech**, is not evidence of realism or passable cancellation, and cannot replace real-world evaluation.

## Ground truth and metrics

- **Echo return loss enhancement (ERLE):** report only under a suitable far-end-only window, with a clear reference baseline and estimator confidence. No ERLE claims from double-talk intervals.
- **Near-end speech fidelity:** compare processed output to clean_voice with sample/time alignment, SI-SDR and appropriate distortion metrics under near-end-only and double-talk conditions. Raw gain reduction is not voice quality.
- **Residual-to-near-end ratio:** with separately known clean and echo stems, compare residual echo versus desired speech.
- **Real-time behavior:** process block deadlines at 48 kHz across realistic VST block sizes (64–2048), callback max/percentile time, heap allocations, jitter, underruns and worst-case CPU.
- **Latency:** track reference buffering, algorithmic lookahead, plugin-reported latency and end-to-end offset independently.
- **Robustness:** count failure recoveries, device switch success, silence pass-through, NaN/infinity, clipping/overflows and sudden path changes.

Targets must be selected from reproducible prototype measurements before enforcing numeric release gates. Display illustrative goals as goals, never results.

## Dataset policy

Keep three separated groups:
1. **Development fixtures:** deterministic synthetic cases and explicitly licensed training/tuning samples.
2. **Held-out evaluation:** separate voices, rooms, devices and transformations that are not used for filter tuning or hyperparameter selection.
3. **Physical acceptance:** consensual, locally recorded headset/mic/room playback with documented setup and no private Discord conversations in the repository.

Record exact source URL/license/consent and SHA-256 for external assets. Keep licensed datasets out of git if redistribution is not allowed; scripted download requires a published license and clear local handling.

## CI sequence

1. Verify file naming, channels, amplitude limits, deterministic seed and timing.
2. Run offline DSP unit tests and block-size/sample-rate matrix once an engine exists.
3. Run regression metrics on stable fixtures and compare against the mainline baseline.
4. Upload compact reports and optionally audio clips for manual listening; do not upload private call audio.
5. Nightly/extended tests broaden random seeds and acoustic paths with no metric cherry-picking.
6. Physical Wave Link/Windows acceptance remains a separate gate until explicitly automated on attached hardware.

## Current state

The Python generator and structural tests are an initial **test-lab bootstrap only**. The AEC executable, scorer, objective baseline and verified real-speech corpus are still to be implemented.
