#!/usr/bin/env python3
"""Score actual processed PCM16 WAV against deterministic fixture ground truth.

Example: python3 tools/score.py --fixture build/fixtures/baseline \
    --processed build/processed.wav --latency-samples 0

Latency is a required nonnegative integer: processed[t + latency] corresponds to
fixture[t]. The processed WAV must contain frame_count + latency frames, including
its flushed tail. No lag search, gain correction, resampling or truncation occurs.
Schema-2 manifest evaluation_windows are half-open [start_sample, end_sample) on
the input timebase, with kind far_only, near_only or double_talk. Metrics aggregate
all windows of each kind. ERLE = 10 log10(dry mic energy / output energy) only in
validated far-only windows; noise contributes to both energies. Nonlinear/clipped
mic windows have no ERLE claim. Near-only and double-talk report projection gain,
error SNR and scale-invariant SDR separately, against clean_voice. These are
synthetic signal metrics, not perceptual or real-world acceptance evidence.
Null dB values have explicit statuses (silence, perfect or invalid); JSON never
contains NaN or Infinity. SI-SDR does not subtract the mean. No pass gates implied.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import wave
from pathlib import Path


class ScoreError(ValueError):
    """An input violates the scoring contract."""


def read_wav(path: Path, channels: int, rate: int, frames: int) -> list[float]:
    try:
        with wave.open(str(path), "rb") as wav:
            if (
                wav.getnchannels(),
                wav.getsampwidth(),
                wav.getframerate(),
                wav.getnframes(),
                wav.getcomptype(),
            ) != (channels, 2, rate, frames, "NONE"):
                raise ScoreError(
                    f"{path.name}: expected {channels}-channel PCM16, {rate} Hz, {frames} frames"
                )
            data = wav.readframes(frames)
    except (wave.Error, EOFError) as exc:
        raise ScoreError(f"{path.name}: invalid PCM WAV: {exc}") from exc
    if len(data) != frames * channels * 2:
        raise ScoreError(f"{path.name}: truncated PCM data")
    return [value / 32768.0 for value in struct.unpack(f"<{frames * channels}h", data)]


def energy(samples: list[float]) -> float:
    return math.fsum(value * value for value in samples)


def ratio_db(numerator: float, denominator: float) -> dict:
    if numerator == 0:
        return {"db": None, "status": "silent_target"}
    if denominator == 0:
        return {"db": None, "status": "perfect"}
    return {"db": 10 * math.log10(numerator / denominator), "status": "finite"}


def fidelity(target: list[float], output: list[float]) -> dict:
    target_energy = energy(target)
    if target_energy == 0:
        return {
            "gain": None,
            "snr": {"db": None, "status": "silent_target"},
            "si_sdr": {"db": None, "status": "silent_target"},
        }
    gain = math.fsum(a * b for a, b in zip(target, output)) / target_energy
    error = energy([a - b for a, b in zip(target, output)])
    projected = [gain * sample for sample in target]
    residual = energy([a - b for a, b in zip(projected, output)])
    si_sdr = ratio_db(energy(projected), residual)
    if energy(output) == 0:
        si_sdr = {"db": None, "status": "silent_output"}
    elif gain == 0:
        si_sdr = {"db": None, "status": "orthogonal_output"}
    return {"gain": gain, "snr": ratio_db(target_energy, error), "si_sdr": si_sdr}


def strict_integer(value: object, field: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise ScoreError(f"{field} must be an integer >= {minimum}")
    return value


def score(fixture: Path, processed: Path, latency_samples: int) -> dict:
    latency = strict_integer(latency_samples, "latency_samples")
    manifest = json.loads((fixture / "manifest.json").read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        raise ScoreError("manifest must be a JSON object")
    if manifest.get("schema") != 2:
        raise ScoreError("schema 2 with explicit evaluation_windows is required")
    rate = strict_integer(manifest.get("sample_rate_hz"), "sample_rate_hz", 1)
    count = strict_integer(manifest.get("frame_count"), "frame_count", 1)
    # Validate all stems, including the stereo reference, before computing metrics.
    reference = read_wav(fixture / "reference.wav", 2, rate, count)
    clean = read_wav(fixture / "clean_voice.wav", 1, rate, count)
    echo = read_wav(fixture / "echo_only.wav", 1, rate, count)
    mic = read_wav(fixture / "mic_input.wav", 1, rate, count)
    output = read_wav(processed, 1, rate, count + latency)[latency:]
    windows = manifest.get("evaluation_windows")
    if not isinstance(windows, list):
        raise ScoreError("evaluation_windows must be a list")
    if type(manifest.get("nonlinear_mic_clip", False)) is not bool:
        raise ScoreError("nonlinear_mic_clip must be a boolean")
    groups = {kind: [] for kind in ("far_only", "near_only", "double_talk")}
    occupied = []
    for window in windows:
        if (
            not isinstance(window, dict)
            or not isinstance(window.get("kind"), str)
            or window["kind"] not in groups
        ):
            raise ScoreError("invalid evaluation window kind")
        start = strict_integer(window.get("start_sample"), "start_sample")
        end = strict_integer(window.get("end_sample"), "end_sample", 1)
        if not start < end <= count:
            raise ScoreError("evaluation window is empty or outside fixture")
        if any(
            start < other_end and end > other_start
            for other_start, other_end in occupied
        ):
            raise ScoreError("evaluation windows overlap")
        occupied.append((start, end))
        kind = window["kind"]
        near_energy = energy(clean[start:end])
        echo_energy = energy(echo[start:end])
        far_energy = energy(reference[2 * start : 2 * end])
        if kind == "far_only" and (
            near_energy != 0 or echo_energy == 0 or far_energy == 0
        ):
            raise ScoreError(
                "far_only requires silent clean_voice and active echo/reference"
            )
        if kind == "near_only" and (
            near_energy == 0 or echo_energy != 0 or far_energy != 0
        ):
            raise ScoreError(
                "near_only requires active clean_voice and silent echo/reference"
            )
        if kind == "double_talk" and (
            near_energy == 0 or echo_energy == 0 or far_energy == 0
        ):
            raise ScoreError(
                "double_talk requires active clean_voice, echo and reference"
            )
        groups[kind].extend(range(start, end))
    report = {
        "schema": 1,
        "scenario": manifest.get("scenario"),
        "sample_rate_hz": rate,
        "frame_count": count,
        "latency_samples": latency,
        "metrics": {},
        "limitations": [
            "Synthetic metrics do not establish perceptual quality or physical acceptance.",
            "Double-talk error includes residual echo; it does not isolate voice distortion.",
        ],
    }
    for kind, indices in groups.items():
        if not indices:
            report["metrics"][kind] = {"sample_count": 0, "status": "no_windows"}
            continue
        dry = [mic[i] for i in indices]
        actual = [output[i] for i in indices]
        desired = [clean[i] for i in indices]
        dry_clips = sum(
            value >= 32767 / 32768 or value <= -32767 / 32768 for value in dry
        )
        output_clips = sum(
            value >= 32767 / 32768 or value <= -32767 / 32768 for value in actual
        )
        result = {
            "sample_count": len(indices),
            "dry_clip_samples": dry_clips,
            "output_clip_samples": output_clips,
        }
        if kind == "far_only":
            if dry_clips or manifest.get("nonlinear_mic_clip", False):
                result["erle"] = {"db": None, "status": "invalid_nonlinear_mic"}
            else:
                result["erle"] = ratio_db(energy(dry), energy(actual))
        else:
            result["dry"] = fidelity(desired, dry)
            result["processed"] = fidelity(desired, actual)
        report["metrics"][kind] = result
    return report


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--processed", required=True, type=Path)
    parser.add_argument("--latency-samples", required=True, type=int)
    parser.add_argument(
        "--out", type=Path, help="Write JSON report here; otherwise print to stdout"
    )
    args = parser.parse_args()
    try:
        result = score(args.fixture, args.processed, args.latency_samples)
    except (ScoreError, OSError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    encoded = json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
