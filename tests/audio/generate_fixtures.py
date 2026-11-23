#!/usr/bin/env python3
"""Reproducible synthetic acoustic echo fixtures. NOT real speech or a DSP quality test."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import struct
import wave
from pathlib import Path

SCENARIOS = (
    "baseline",
    "headphones-on-mic",
    "double-talk",
    "long-delay",
    "gain-change",
    "path-change",
    "reference-silent",
    "nonlinear-clip",
    "drift",
)


def clamp(x: float) -> float:
    return max(-1.0, min(1.0, x))


def write_mono(path: Path, samples: list[float], sample_rate: int) -> None:
    pcm = [int(round(clamp(x) * 32767)) for x in samples]
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(struct.pack("<" + "h" * len(pcm), *pcm))


def write_stereo(
    path: Path, left: list[float], right: list[float], sample_rate: int
) -> None:
    pcm = []
    for l, r in zip(left, right):
        pcm.extend((int(round(clamp(l) * 32767)), int(round(clamp(r) * 32767))))
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(struct.pack("<" + "h" * len(pcm), *pcm))


def speech_like(n: int, sr: int, base_hz: float, seed: int) -> list[float]:
    """Synthesize deterministic modulated harmonics; not actual human speech."""
    rng = random.Random(seed)
    phases = [rng.uniform(-math.pi, math.pi) for _ in range(5)]
    result = []
    for i in range(n):
        t = i / sr
        syllables = 0.2 + 0.8 * max(0.0, math.sin(2 * math.pi * 3.5 * t)) ** 2
        pauses = 0.0 if int(t * 1.7) % 5 == 4 else 1.0
        f0 = base_hz * (1.0 + 0.06 * math.sin(2 * math.pi * 1.3 * t))
        voiced = (
            sum(
                math.sin(2 * math.pi * f0 * (k + 1) * t + phases[k]) / (k + 1)
                for k in range(5)
            )
            / 2.25
        )
        result.append(0.36 * pauses * syllables * voiced)
    return result


def generate_scenario(
    scenario: str,
    out_dir: Path,
    sr: int = 48000,
    seconds: float = 3.0,
    seed: int = 2026,
) -> dict:
    if scenario not in SCENARIOS:
        raise ValueError(f"Unknown scenario: {scenario}")
    if (
        not isinstance(sr, int)
        or sr < 8000
        or not math.isfinite(seconds)
        or seconds <= 0
    ):
        raise ValueError(
            "sample rate must be an integer >= 8000 and seconds finite and > 0"
        )
    n = round(seconds * sr)
    if n < 1:
        raise ValueError("duration must produce at least one sample")
    out_dir.mkdir(parents=True, exist_ok=True)
    far = speech_like(n, sr, 165.0, seed + 1)
    near = speech_like(n, sr, 121.0, seed + 2)
    # These schedules provide calibration, overlap and echo-tail-free voice windows.
    far_stop = 7 * n // 10
    near_start = 4 * n // 10
    far[far_stop:] = [0.0] * (n - far_stop)
    near[:near_start] = [0.0] * near_start
    if scenario == "reference-silent":
        near_start = 0
        near = speech_like(n, sr, 121.0, seed + 2)
        far = [0.0] * n
    if scenario == "baseline":
        near = [0.0] * n
    if scenario == "headphones-on-mic":
        base_delay = round(sr * 0.006)
        gain = 1.5
        taps = [(0, 0.93), (round(sr * 0.004), 0.28), (round(sr * 0.018), -0.19)]
    elif scenario == "long-delay":
        base_delay = round(sr * 0.24)
        gain = 0.62
        taps = [(0, 0.85), (round(sr * 0.035), 0.3)]
    else:
        base_delay = round(sr * 0.023)
        gain = 0.68
        taps = [(0, 0.75), (round(sr * 0.012), 0.34), (round(sr * 0.047), -0.19)]
    # Mild stereo difference gives future render-path logic a meaningful input.
    left = far
    right = [far[i - 3] * 0.91 if i >= 3 else 0.0 for i in range(n)]
    render = [0.64 * left[i] + 0.36 * right[i] for i in range(n)]
    changed_taps = [(0, 0.45), (round(sr * 0.008), -0.42), (round(sr * 0.031), 0.28)]
    drift_ppm = 533.3333333333334 if scenario == "drift" else 0.0
    echo = []
    for i in range(n):
        extra_delay = i * drift_ppm / 1_000_000
        cur_gain = gain * (1.65 if scenario == "gain-change" and i >= n // 2 else 1.0)
        active_taps = (
            changed_taps if scenario == "path-change" and i >= n // 2 else taps
        )
        sample = 0.0
        for delay, amplitude in active_taps:
            source = i - base_delay - extra_delay - delay
            lower = math.floor(source)
            fraction = source - lower
            first = render[lower] if 0 <= lower < n else 0.0
            second = render[lower + 1] if 0 <= lower + 1 < n else 0.0
            sample += amplitude * (first + fraction * (second - first))
        echo.append(sample * cur_gain)
    rng = random.Random(seed + 3)
    noise = [rng.uniform(-0.0005, 0.0005) for _ in range(n)]
    mixture = [near[i] + echo[i] + noise[i] for i in range(n)]
    preamp = 8.0 if scenario == "nonlinear-clip" else 1.0
    output_gain = 0.72 if scenario == "nonlinear-clip" else 1.0
    clipped_count = sum(abs(x * preamp) > 1.0 for x in mixture)
    mic = [clamp(x * preamp) * output_gain for x in mixture]
    max_tap_delay = max(d for d, _ in taps + changed_taps)
    tail_end = min(
        n,
        far_stop
        + 3
        + base_delay
        + max_tap_delay
        + math.ceil((n - 1) * drift_ppm / 1_000_000)
        + 2,
    )
    echo_start = min(n, base_delay + max_tap_delay + 2)
    windows = []

    def window(kind: str, start: int, end: int) -> None:
        if end > start:
            windows.append({"kind": kind, "start_sample": start, "end_sample": end})

    if scenario == "reference-silent":
        window("near_only", 0, n)
    elif scenario == "baseline":
        window("far_only", echo_start, far_stop)
    else:
        window("far_only", echo_start, min(near_start, far_stop))
        window("double_talk", max(near_start, echo_start), far_stop)
        window("near_only", max(near_start, tail_end), n)
    write_stereo(out_dir / "reference.wav", left, right, sr)
    write_mono(out_dir / "clean_voice.wav", near, sr)
    write_mono(out_dir / "echo_only.wav", echo, sr)
    write_mono(out_dir / "mic_input.wav", mic, sr)
    metadata = {
        "schema": 2,
        "dataset_split": "development",
        "evaluation_windows": windows,
        "evaluation_timebase": "microphone input samples; half-open intervals",
        "scenario": scenario,
        "synthetic_only": True,
        "generator_seed": seed,
        "sample_rate_hz": sr,
        "frame_count": n,
        "duration_seconds": n / sr,
        "channels": {
            "reference.wav": 2,
            "clean_voice.wav": 1,
            "echo_only.wav": 1,
            "mic_input.wav": 1,
        },
        "reference_to_mic_base_delay_samples": base_delay,
        "acoustic_gain": gain,
        "path_taps": [{"extra_delay_samples": d, "gain": g} for d, g in taps],
        "nonlinear_mic_clip": scenario == "nonlinear-clip",
        "microphone_processing": {
            "operation": "output_gain * clamp(preamp * (clean_voice + echo_only + noise), -1, 1)",
            "preamp": preamp,
            "output_gain": output_gain,
            "clipped_sample_count": clipped_count,
        },
        "noise": {
            "distribution": "uniform",
            "peak_amplitude": 0.0005,
            "seed": seed + 3,
        },
        "source_activity": {
            "far_end_stop_sample": far_stop,
            "near_end_start_sample": near_start,
        },
        "reference_mix_weights": [0.64, 0.36],
        "reference_right_delay_samples": 3,
        "reference_right_gain": 0.91,
        "drift_ppm": drift_ppm,
        "drift_samples_end": (n - 1) * drift_ppm / 1_000_000,
        "drift_model": "source_sample = mic_sample - base_delay - tap_delay - mic_sample * drift_ppm / 1000000; linear interpolation, zero outside source",
        "gain_change": (
            {"sample": n // 2, "multiplier": 1.65}
            if scenario == "gain-change"
            else None
        ),
        "path_change": (
            {
                "sample": n // 2,
                "path_taps": [
                    {"extra_delay_samples": d, "gain": g} for d, g in changed_taps
                ],
            }
            if scenario == "path-change"
            else None
        ),
        "encoding": "signed 16-bit little-endian PCM; round(clamp(sample) * 32767)",
        "provenance": {
            "source": "Project-generated modulated harmonics and pseudorandom noise; no recordings or external media",
            "signal_type": "synthetic, not human speech",
            "generator": "tests/audio/generate_fixtures.py",
            "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "license": "MIT (project-authored fixture synthesis)",
        },
        "note": "Near-end is zero in baseline; reference is zero in reference-silent. Audio is synthetic, not speech recordings.",
    }
    metadata["wav_sha256"] = {
        filename: hashlib.sha256((out_dir / filename).read_bytes()).hexdigest()
        for filename in metadata["channels"]
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return metadata


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("build/fixtures"))
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--seconds", type=float, default=3.0)
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument(
        "--scenarios", nargs="+", default=list(SCENARIOS), choices=SCENARIOS
    )
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    index = [
        generate_scenario(
            name, args.out / name, args.sample_rate, args.seconds, args.seed
        )
        for name in args.scenarios
    ]
    (args.out / "index.json").write_text(
        json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"Generated {len(index)} deterministic scenarios in {args.out}")


if __name__ == "__main__":
    main()
