# Synthetic audio fixtures

Generate nine deterministic acoustic echo scenarios using the standard library:

```sh
python3 tests/audio/generate_fixtures.py --out build/fixtures --seconds 3
python3 -m unittest discover -s tests/audio -v
```

The existing `--sample-rate`, `--seconds`, `--seed`, and `--scenarios` options
select reproducible parameters. Each scenario emits stereo `reference.wav`, mono
`clean_voice.wav`, `echo_only.wav`, `mic_input.wav`, and `manifest.json`.
The index repeats each manifest. Signals share the microphone sample rate and
frame count; the reference is on its original playback source time axis. PCM is
signed 16-bit little endian, encoded as `round(clamp(sample) * 32767)`.

The source is project-authored modulated harmonics and seeded noise, licensed
under the project's MIT license. No external media, recorded voices or identifying
audio is used. Manifests record the generator source SHA-256, individual WAV
SHA-256 hashes, seeds and synthesis parameters. Byte reproducibility applies to
the same generator revision, parameters and Python runtime/platform; floating
point trigonometry can vary between platforms.

All generated fixtures are explicitly marked `dataset_split: development`.
These scenarios are intended for implementation and tuning. Different random
seeds from this same generator do not establish an independently held-out corpus;
licensed real speech, unseen measured paths and physical acceptance remain
separate outstanding work.

Schema 2 adds `evaluation_windows`, each containing `kind`, `start_sample`, and
`end_sample`. Kinds are `far_only`, `near_only`, and `double_talk`. Intervals are
half-open and use the microphone input time axis. The generator schedules far-end
playback in the first 70% and near-end voice from 40% onward. Baseline has no
near-end voice; reference-silent has near-end voice throughout and no playback.
Windows exclude initial path filling and the complete echo tail before labeling
near-only activity. Very short fixtures can omit a window when the physical delay
leaves no usable interval. Window activity is checked against emitted clean/echo
stems; these labels do not certify successful cancellation or alignment confidence.

The echo is a documented weighted stereo render mixture through delayed taps.
Headphones-on-mic uses a louder short path, long-delay uses a 240 ms base delay,
gain-change switches acoustic gain at the midpoint, and path-change switches tap
coefficients/delays there. Drift uses a fixed 533.333 ppm increasing offset with
linear interpolation, so its final offset depends on duration. Nonlinear-clip
applies an 8x microphone preamp, hard clipping at ±1 and a 0.72 output gain after
combining clean voice, echo and seeded noise. The echo stem remains the acoustic
contamination before microphone processing. The manifest records actual clipped
sample count; ordinary scenarios are checked to avoid accidental clipping.

Tests reconstruct echo from the emitted reference and declared transformation,
reconstruct microphone mixture within PCM quantization error, verify window stem
activity, confirm channels/timing and compare SHA-256 across two clean generations
of every scenario. These checks validate the fixture model, not its physical
realism. Synthetic harmonic audio is **not natural speech**, a substitute for real
data or evidence that BunCancel actually cancels echo. See `docs/TEST_PLAN.md`.
