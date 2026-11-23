# Offline development measurements

Cold-start runs on nine three-second synthetic development fixtures at 48 kHz, seed 2026. Each process starts with an empty filter and receives 256-frame blocks, including a final partial block. The microphone has zero algorithmic lookahead. The scorer aligns emitted PCM16 at zero samples and includes acquisition in the labeled intervals. No clean stem, delay label or activity label enters the engine.

| Scenario | Far-only ERLE (dB) | SpeexDSP far-only ERLE (dB) | Dry double-talk SNR (dB) | Output double-talk SNR (dB) | Output double-talk projection gain | Near-only output SNR (dB) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | 2.10 | 7.16 | — | — | — | — |
| headphones-on-mic | 1.34 | 8.56 | -2.77 | -2.44 | 1.01 | 41.33 |
| double-talk | 1.07 | 6.32 | 4.81 | 6.51 | 0.94 | 41.39 |
| long-delay | 0.78 | 2.16 | 6.04 | 6.71 | 0.99 | 38.48 |
| gain-change | 1.07 | 6.32 | 1.75 | 4.29 | 0.94 | 41.39 |
| path-change | 1.07 | 6.32 | 5.71 | 6.43 | 0.92 | 41.39 |
| reference-silent | — | — | — | — | — | 46.47 |
| nonlinear-clip | — | — | -12.75 | -11.84 | 4.09 | -11.80 |
| drift | 0.75 | 5.25 | 4.82 | 4.04 | 0.96 | 41.39 |

Dry-mic ERLE is 0 dB wherever defined. A dash marks an unavailable window or invalid ERLE under nonlinear clipping. Far-only ERLE measures dry-mic energy divided by processed energy, including the fixture noise floor. Near and double-talk SNR use the clean voice directly as the target, so residual playback, changes to voice and noise all contribute to the error. A projection gain near 1 can coexist with audible residual echo. The full JSON includes window sample counts, SI-SDR, dry and processed gains, clipping counts and timing.

The 240 ms and drifting paths achieved less than 1 dB of far-only attenuation. Under drift, double-talk SNR fell from 4.82 to 4.04 dB. The nonlinear fixture produced 59 clipped output samples; the microphone preamp and hard clipping also distort the dry signal. On reference-silent, the output reproduces the input PCM exactly.

## Engine and comparator

The engine searches for a positive delay up to one third of a second and fits a 128-tap mono normalized LMS filter. At 48 kHz the adaptive tail spans 2.67 ms after the selected delay. Several fixture reflections arrive tens of milliseconds later. The alignment search takes time at startup, ambiguous periodic signals can remain dry, and independent clocks can move the echo outside the learned filter. Stereo playback uses a 50/50 downmix. There is no frequency-domain tail, active clock resampling or residual postfilter in this baseline.

The first delay estimator measured 0 dB on the baseline fixture. The revised search spreads correlation samples across a longer interval and checks the proposed lag against newer observations. Near-end protection also limits adaptation and subtraction when the predicted echo and mic disagree. The full matrix above includes gain changes, moving paths and clipping.

The separate SpeexDSP 1.2.1 comparator uses the Debian 1.2.1-3 BSD-3-Clause shared library, 10 ms synchronous frames, a 320 ms filter, identical 50/50 reference mix and no preprocessor or oracle alignment. It is one fixed comparison configuration. Both algorithms see each input from its beginning. Near-end metrics and comparison audio for this configuration are in the local evidence archive. The product has no dependency on SpeexDSP. [Comparator API](https://www.speex.org/docs/manual/speex-manual/node7.html)

## Reproduction

Run the Release build, lab and optional comparator commands in the README. The lab saves all source stems, processed WAVs and metrics alongside manifest hashes. The Linux CI artifact contains the BunCancel A/B pairs. The independent comparison requires the system comparator library.

Local Linux measurements used GCC 14.2 and Python 3.13.5. Across these nine offline runs, the largest 99th-percentile processing time was 548 µs and the largest block time was 653 µs. The runner counted 0 block deadline misses against the actual frame count. These times exclude WAV I/O, device handling and host scheduling. The allocation test covers replaceable C++ allocation functions during prepared DSP processing and reset.

These are development measurements on generated signals. Held-out measured room paths, conversational speech, physical routing and Windows microphone-to-output latency remain unverified.
