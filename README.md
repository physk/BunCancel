# 🐰 BunCancel

**Keep your voice. Lose the echo.**

BunCancel is an experimental, free and open-source Windows VST3 acoustic echo cancellation (AEC) plugin. It aims to remove audio leaking from speakers or headphones into a microphone **using a known playback reference**, while preserving the person speaking into the mic.

The first target is gaming and voice chat: play a friend's voice through headphones, even close to the microphone, without sending that voice back through the mic feed. The initial reference scenario is **Wave Link 3 Personal Mix → BunCancel reference capture → microphone VST3 processing → clean microphone output**. Other audio devices and routing software are first-class targets.

> **Experimental.** The portable engine processes WAV files and the development lab measures the result. The Windows plugin requires host and physical audio qualification before use on calls.

![BunCancel logo](assets/logo.svg)

## Product goals

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

## Run the offline engine

Use CMake 3.20+, a C++20 compiler and Python 3.10+. From the repository root on Linux:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
python3 -m unittest discover -s tests/audio -v
python3 tools/run_lab.py --runner build/linux/buncancel-offline --out build/evaluation
```

Listen to `mic_input.wav` and `processed.wav` in the same scenario directory. Compare them with the isolated `clean_voice.wav` and `echo_only.wav`. The lab writes paired scores and timing diagnostics to `build/evaluation/metrics.json`. To process your own equal-rate, equal-length pair:

```sh
build/linux/buncancel-offline reference.wav mic.wav processed.wav 256
```

The optional independent comparison uses the system SpeexDSP shared library only in a separate offline process:

```sh
python3 tools/compare_speex.py build/evaluation
```

See [development measurements](docs/BASELINE.md) for results and failure cases and [experimental Windows VST3](docs/VST3.md) for host/build constraints.

## Design principles

1. **Original AEC DSP**: implement the adaptive echo estimator, delay estimator, double-talk handling and residual suppressor here. Research papers and established engines can inform benchmarks; do not copy an AEC implementation.
2. **Evidence over demos**: every algorithm change needs repeatable tests, quantitative reports and, where relevant, sound examples.
3. **Do not block the audio callback**: no WASAPI calls, disk access, locks, sleeps, memory allocation, device enumeration or UI activity on the real-time path.
4. **Privacy by default**: audio stays local; no telemetry or cloud processing.
5. **Honest performance**: specify latency, CPU limits, quality regimes and unresolved cases.

## Roadmap

Physical Wave Link and wireless-headphone qualification, independent speech recordings, clock reconciliation and longer acoustic paths remain open. See the [roadmap](docs/ROADMAP.md) for the acceptance gates.

## Building and contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). This repository is available under the [MIT license](LICENSE) for original project-authored material; dependencies retain their own licenses.
