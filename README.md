# 🐰 BunCancel

**Keep your voice. Lose the echo.**

BunCancel is an experimental, free and open-source Windows VST3 acoustic echo cancellation (AEC) plugin. It aims to remove audio leaking from speakers or headphones into a microphone **using a known playback reference**, while preserving the person speaking into the mic.

The first target is gaming and voice chat: play a friend's voice through headphones, even close to the microphone, without sending that voice back through the mic feed. The initial reference scenario is **Wave Link 3 Personal Mix → BunCancel reference capture → microphone VST3 processing → clean microphone output**. Other audio devices and routing software are first-class targets.

> **Status: design and reproducible test-lab bootstrap. No working AEC implementation or verified cancellation claims yet.** Do not advertise the plugin as ready for calls until the real-time host and acoustic tests pass.

![BunCancel logo](assets/logo.svg)

## What it does

- Uses a clean render/playback feed as the **far-end reference**; it is never mixed into the mic output.
- Learns acoustic transfer paths, playback-to-microphone delay and changing room/headphone conditions.
- Removes reference-correlated acoustic bleed while retaining near-end voice, including during simultaneous speech (double-talk).
- Works with a selectable Windows audio endpoint; Wave Link is a primary test case, **not a hard dependency**.
- Targets low and deterministic latency, real-time stability and predictable behavior when reference capture fails.
- Will provide clear, measurable quality tests and before/after audio samples.

**AEC is not a noise gate, voice isolation model, or digital-minus-one mixer.** It cannot guarantee perfect removal, especially with clipping, highly nonlinear speaker distortion, or an invalid reference.

## Project documents

| Read | Purpose |
| --- | --- |
| [Product vision](docs/VISION.md) | Why BunCancel exists and the experience it should provide |
| [Requirements](docs/REQUIREMENTS.md) | Required behavior, scope, acceptance checks and failure modes |
| [Architecture](docs/ARCHITECTURE.md) | Signal flow, capture boundaries, original DSP design and threading |
| [Automated test plan](docs/TEST_PLAN.md) | Deterministic fixtures, quality metrics and acoustic torture tests |
| [Roadmap](docs/ROADMAP.md) | Delivery sequence and definition of done |
| [Agent instructions](AGENTS.md) | Rules for coding agents and maintainers |

## Getting started with the test lab

Requires Python 3.10+; the initial fixture generator uses only the standard library.

1. Run: **python tests/audio/generate_fixtures.py --out build/fixtures**
2. Inspect the WAV files in each scenario directory: **reference.wav**, **mic_input.wav**, **clean_voice.wav** and **echo_only.wav**.
3. Run: **python -m unittest discover -s tests/audio -p "test_*.py" -v**

The audio is synthetic and deterministic. It is useful for CI, isolation and algorithm debugging, but **must not be mistaken for a representative real speech corpus**. Publicly redistributable clean speech and measured acoustic impulse responses will be added with documented provenance and licenses. Do not commit captures of private conversations.

## Design principles

1. **Original AEC DSP**: implement the adaptive echo estimator, delay estimator, double-talk handling and residual suppressor here. Research papers and established engines can inform benchmarks; do not copy an AEC implementation.
2. **Evidence over demos**: every algorithm change needs repeatable tests, quantitative reports and, where relevant, sound examples.
3. **Do not block the audio callback**: no WASAPI calls, disk access, locks, sleeps, memory allocation, device enumeration or UI activity on the real-time path.
4. **Privacy by default**: audio stays local; no telemetry or cloud processing.
5. **Honest performance**: specify latency, CPU limits, quality regimes and unresolved cases.

## Roadmap

First: establish deterministic test fixtures and offline evaluation; then build a reference-capture proof of concept and a minimal VST3 shell. Next implement and validate the echo estimator and double-talk behavior. Finally test Wave Link 3 with actual wireless headphones against both simulated and physical acoustic paths.

See [Roadmap](docs/ROADMAP.md) for detailed gates.

## Building and contributing

The planned implementation uses **C++20, CMake and the Steinberg VST3 SDK** (subject to review of the exact SDK version and its license). Windows/WASAPI integration will be isolated from portable DSP code. No third-party AEC engine will be embedded or forked. Builds and dependencies should be pinned and reproducible.

See [CONTRIBUTING.md](CONTRIBUTING.md). This repository is available under the [MIT license](LICENSE) for original project-authored material; dependencies retain their own licenses.
