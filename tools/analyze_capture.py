#!/usr/bin/env python3
"""Inspect an explicitly recorded local BunCancel diagnostic without uploading it.

The report measures observed levels, sample changes, availability and possible
lag. Unlabelled microphone audio has no clean near/echo stems, so this tool never
reports ERLE or voice-preservation quality. Optional native replay starts with an
empty filter for each uninterrupted usable-reference segment and uses temporary
files removed before exit. Its PCM16 output differs in precision and initial
state from the captured float32 host output.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import subprocess
import tempfile
from pathlib import Path

SHADOW_DECISIONS = {
    0: "waiting for alignment",
    1: "training a candidate filter",
    2: "validating frozen candidate coefficients",
    3: "insufficient eligible training samples",
    4: "insufficient eligible validation evidence",
    5: "candidate prediction has weak coherence",
    6: "candidate did not sufficiently improve on the microphone",
    7: "current output already predicts the microphone better",
    8: "validated candidate promoted",
}


class CaptureError(ValueError):
    """The local recording is incomplete or inconsistent."""


def integer(value: object, name: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise CaptureError(f"{name} must be an integer >= {minimum}")
    return value


def read_wav(path: Path) -> tuple[int, list[float]]:
    size = path.stat().st_size
    if size < 44 or size > 32 * 1024 * 1024:
        raise CaptureError(f"{path.name}: unsupported diagnostic WAV size")
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise CaptureError(f"{path.name}: expected little-endian RIFF WAV")
    if struct.unpack_from("<I", data, 4)[0] != len(data) - 8:
        raise CaptureError(f"{path.name}: incomplete RIFF data")
    format_info = None
    payload = None
    position = 12
    while position < len(data):
        if len(data) - position < 8:
            raise CaptureError(f"{path.name}: truncated chunk header")
        tag, count = struct.unpack_from("<4sI", data, position)
        position += 8
        if position + count + (count & 1) > len(data):
            raise CaptureError(f"{path.name}: truncated chunk")
        chunk = data[position : position + count]
        if tag == b"fmt ":
            if format_info is not None or count < 16:
                raise CaptureError(f"{path.name}: invalid format chunk")
            format_info = struct.unpack_from("<HHIIHH", chunk)
        elif tag == b"data":
            if payload is not None:
                raise CaptureError(f"{path.name}: duplicate audio chunk")
            payload = chunk
        position += count + (count & 1)
    if format_info is None or payload is None:
        raise CaptureError(f"{path.name}: missing audio or format")
    encoding, channels, rate, byte_rate, alignment, bits = format_info
    if (
        channels != 1
        or not 8000 <= rate <= 192000
        or (encoding, bits) not in {(3, 32), (1, 16)}
        or alignment != bits // 8
        or byte_rate != rate * alignment
        or not payload
        or len(payload) % alignment
    ):
        raise CaptureError(f"{path.name}: expected mono float32 or PCM16 WAV")
    code = "f" if encoding == 3 else "h"
    values = list(struct.unpack(f"<{len(payload) // alignment}{code}", payload))
    if encoding == 1:
        values = [sample / 32768 for sample in values]
    if not all(math.isfinite(sample) for sample in values):
        raise CaptureError(f"{path.name}: nonfinite audio")
    return rate, values


def write_float_wav(path: Path, rate: int, values: list[float]) -> None:
    payload = struct.pack(f"<{len(values)}f", *values)
    path.write_bytes(
        b"RIFF"
        + struct.pack("<I", len(payload) + 36)
        + b"WAVEfmt "
        + struct.pack("<IHHIIHH", 16, 3, 1, rate, rate * 4, 4, 32)
        + b"data"
        + struct.pack("<I", len(payload))
        + payload
    )


def power(values: list[float]) -> float:
    return math.fsum(sample * sample for sample in values)


def level(values: list[float]) -> dict:
    rms = math.sqrt(power(values) / len(values)) if values else 0
    peak = max((abs(sample) for sample in values), default=0)
    return {
        "rms_dbfs": 20 * math.log10(rms) if rms else None,
        "peak_dbfs": 20 * math.log10(peak) if peak else None,
        "exact_silence": peak == 0,
    }


def comparison(mic: list[float], output: list[float]) -> dict:
    input_power, output_power = power(mic), power(output)
    delta = [a - b for a, b in zip(mic, output)]
    return {
        "changed_frames": sum(a != b for a, b in zip(mic, output)),
        "frames": len(mic),
        "delta": level(delta),
        "observed_mic_output_level_change_db": (
            10 * math.log10(input_power / output_power)
            if input_power and output_power
            else None
        ),
    }


def lag_probe(mic: list[float], reference: list[float], rate: int) -> dict:
    """Bounded exploratory sparse-difference search, independent of DSP tuning."""
    max_lag = min(rate // 2, len(mic) // 4)
    span = min(rate // 5, len(mic) - 2 * max_lag - 2)
    if span < 128:
        return {"status": "insufficient_audio"}
    # Evaluate three later excerpts so one periodic passage cannot establish a
    # persistent path by itself. No threshold here changes the plugin's gate.
    windows = []
    for fraction in (0.4, 0.65, 0.9):
        end = min(
            len(mic) - max_lag - 1, max(max_lag + span + 1, int(len(mic) * fraction))
        )
        positions = [end - 1 - i * span // 128 for i in range(128)]
        microphone = [mic[t] - mic[t - 1] for t in positions]
        mic_energy = power(microphone)

        def score(lag: int) -> float:
            far = [reference[t - lag] - reference[t - lag - 1] for t in positions]
            far_energy = power(far)
            if not mic_energy or not far_energy:
                return 0
            cross = math.fsum(a * b for a, b in zip(microphone, far))
            return min(1.0, abs(cross) / math.sqrt(mic_energy * far_energy))

        # Sparse first differences have narrow peaks. Probe every lag with a
        # bounded 32-point coarse score, then fully test the strongest 24 lags.
        sparse_positions = positions[::4]
        sparse_mic = microphone[::4]
        sparse_energy = power(sparse_mic)
        candidates = []
        for lag in range(-max_lag, max_lag + 1):
            far = [
                reference[t - lag] - reference[t - lag - 1] for t in sparse_positions
            ]
            far_energy = power(far)
            cross = math.fsum(a * b for a, b in zip(sparse_mic, far))
            value = (
                abs(cross) / math.sqrt(sparse_energy * far_energy)
                if sparse_energy and far_energy
                else 0
            )
            candidates.append((value, lag))
        candidates.sort(reverse=True)
        tested = sorted(((score(lag), lag) for _, lag in candidates[:24]), reverse=True)
        best, lag = tested[0]
        competing = max(
            (value for value, other in tested if abs(other - lag) > 128), default=0
        )
        windows.append(
            {
                "end_seconds": end / rate,
                "lag_samples": lag,
                "lag_ms": lag * 1000 / rate,
                "absolute_correlation": best,
                "competing_correlation": competing,
            }
        )
    return {
        "status": "exploratory",
        "timebase": "recorded DSP-ready reference; lag excludes plugin reference holdback",
        "method": "three 200 ms excerpts, sparse first differences, signed lag up to 500 ms",
        "windows": windows,
        "limitation": "Periodic material, near speech, short windows and acoustic coloration can produce misleading peaks; this is not verified acoustic delay.",
    }


def valid_spans(blocks: list[dict]) -> list[tuple[int, int]]:
    spans = []
    for block in blocks:
        if not block["reference_usable"]:
            continue
        start = block["frame_offset"]
        end = start + block["frames"]
        if spans and spans[-1][1] == start and not block.get("reset", False):
            spans[-1] = (spans[-1][0], end)
        else:
            spans.append((start, end))
    return spans


def pcm16(values: list[float]) -> list[float]:
    # Match std::round in the native writer, including ties away from zero.
    result = []
    for value in values:
        scaled = value * 32768
        rounded = math.floor(scaled + 0.5) if scaled >= 0 else math.ceil(scaled - 0.5)
        result.append(min(32767, max(-32768, rounded)) / 32768)
    return result


def native_replay(
    runner: Path,
    rate: int,
    mic: list[float],
    reference: list[float],
    output: list[float],
    spans: list[tuple[int, int]],
) -> dict:
    results = []
    with tempfile.TemporaryDirectory(prefix="buncancel-replay-") as temporary:
        directory = Path(temporary)
        eligible = [(start, end) for start, end in spans if end - start >= rate // 4]
        for start, end in eligible[:16]:
            write_float_wav(directory / "mic.wav", rate, mic[start:end])
            write_float_wav(directory / "reference.wav", rate, reference[start:end])
            process = subprocess.run(
                [
                    str(runner),
                    str(directory / "reference.wav"),
                    str(directory / "mic.wav"),
                    str(directory / "processed.wav"),
                ],
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            if process.returncode:
                raise CaptureError("native offline runner failed during local replay")
            replay_rate, replay = read_wav(directory / "processed.wav")
            if replay_rate != rate or len(replay) != end - start:
                raise CaptureError("native replay returned inconsistent audio")
            metrics = json.loads(process.stdout)
            results.append(
                {
                    "start_frame": start,
                    "frames": end - start,
                    "alignment_confidence": metrics.get("alignment_confidence"),
                    "delay_samples": metrics.get("delay_samples"),
                    "aligned": metrics.get("aligned"),
                    "replay_vs_pcm16_dry": comparison(pcm16(mic[start:end]), replay),
                    "captured_pcm16_vs_replay": comparison(
                        pcm16(output[start:end]), replay
                    ),
                }
            )
    return {
        "status": "completed",
        "segments": results,
        "skipped_short_segments": len(spans) - len(eligible),
        "unreplayed_segments": max(0, len(eligible) - 16),
        "limitation": "Each valid segment starts with an empty filter. Earlier host training, transport timing, original callback sizes and float32 output precision are not reproduced; replay output is PCM16. Dry and captured samples are quantized to the same PCM16 grid before replay comparisons to avoid counting quantization alone as subtraction.",
    }


def analyze(
    directory: Path, runner: Path | None = None, probe_lag: bool = True
) -> dict:
    metadata_path = directory / "timeline.json"
    if metadata_path.stat().st_size > 16 * 1024 * 1024:
        raise CaptureError("timeline is too large")
    document = json.loads(metadata_path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise CaptureError("expected local diagnostic schema 1")
    if document.get("complete") is not True:
        raise CaptureError("local recording must be complete before analysis")
    rate = integer(document.get("sample_rate"), "sample_rate", 8000)
    frames = integer(document.get("captured_frames"), "captured_frames", 1)
    if rate > 192000 or frames > rate * 4:
        raise CaptureError("local diagnostic exceeds four seconds")
    signals = {}
    for name, filename in (
        ("mic", "mic_input.wav"),
        ("reference", "reference.wav"),
        ("output", "processed.wav"),
    ):
        signal_rate, values = read_wav(directory / filename)
        if signal_rate != rate or len(values) != frames:
            raise CaptureError(f"{filename}: timeline and WAV dimensions differ")
        signals[name] = values
    blocks = document.get("blocks")
    if not isinstance(blocks, list) or not blocks or len(blocks) > frames:
        raise CaptureError("timeline has no bounded block evidence")
    position = 0
    sanitized_blocks = []
    for block in blocks:
        if not isinstance(block, dict):
            raise CaptureError("invalid timeline block")
        start = integer(block.get("frame_offset"), "frame_offset")
        count = integer(block.get("frames"), "block frames", 1)
        if start != position or start + count > frames:
            raise CaptureError("timeline blocks must exactly cover captured frames")
        if type(block.get("reference_usable")) is not bool:
            raise CaptureError("reference_usable must be explicit")
        clean = {
            "frame_offset": start,
            "frames": count,
            "reference_usable": block["reference_usable"],
        }
        for field in (
            "source_block_frames",
            "time_ms",
            "reference_time_ms",
            "reset",
            "status",
            "aligned",
            "subtraction_active",
            "reducing",
            "live_confidence",
            "peak_correlation",
            "active_delay_ms",
            "adapted_samples",
            "shadow_samples",
            "promotions",
            "filter_resets",
            "underruns",
            "dropouts",
            "mic_rms",
            "reference_rms",
            "raw_reference_rms",
            "output_rms",
            "changed_samples",
            "shadow_decision",
            "shadow_validation_count",
            "shadow_training_eligibility",
            "shadow_validation_eligibility",
            "shadow_coherence",
            "shadow_mic_error_ratio",
            "shadow_foreground_error_ratio",
            "reference_receipt_age_ms",
            "window_reference_rms",
            "window_downmix_rms",
            "window_reference_seconds",
        ):
            value = block.get(field)
            if value is not None and (
                type(value) not in (int, float, bool) or not math.isfinite(value)
            ):
                raise CaptureError(f"invalid numeric timeline field {field}")
            if field in block:
                clean[field] = value
        clean["recorded_changed_frames"] = sum(
            a != b
            for a, b in zip(
                signals["mic"][start : start + count],
                signals["output"][start : start + count],
            )
        )
        sanitized_blocks.append(clean)
        position += count
    blocks = sanitized_blocks
    if position != frames:
        raise CaptureError("timeline blocks do not cover captured frames")
    spans = valid_spans(blocks)
    available = sum(end - start for start, end in spans)
    windows = []
    width = max(1, rate // 4)
    for start in range(0, frames, width):
        end = min(frames, start + width)
        windows.append(
            {
                "start_seconds": start / rate,
                "duration_seconds": (end - start) / rate,
                **{name: level(values[start:end]) for name, values in signals.items()},
                **comparison(signals["mic"][start:end], signals["output"][start:end]),
            }
        )
    threshold_dbfs = -60.0
    activity = {}
    for signal in signals:
        active_windows = [
            window
            for window in windows
            if window[signal]["rms_dbfs"] is not None
            and window[signal]["rms_dbfs"] > threshold_dbfs
        ]
        latest = active_windows[-1] if active_windows else None
        activity[signal] = {
            "windows_above_threshold": len(active_windows),
            "window_count": len(windows),
            "last_above_threshold_end_seconds": (
                latest["start_seconds"] + latest["duration_seconds"] if latest else None
            ),
        }
    filter_evidence = {
        "aligned_frames": sum(
            block["frames"] for block in blocks if block.get("aligned") is True
        ),
        "reported_subtraction_frames": sum(
            block["frames"]
            for block in blocks
            if block.get("subtraction_active") is True
        ),
        "reference_reset_blocks": sum(block.get("reset") is True for block in blocks),
        "first_observed_adapted_samples": blocks[0].get("adapted_samples"),
        "initial_state_limitation": "The first observation follows one audio callback. Its update count cannot establish whether training happened before capture started.",
        "first": {
            field: blocks[0].get(field)
            for field in (
                "live_confidence",
                "peak_correlation",
                "adapted_samples",
                "shadow_samples",
                "promotions",
                "filter_resets",
            )
        },
        "last": {
            field: blocks[-1].get(field)
            for field in (
                "live_confidence",
                "peak_correlation",
                "adapted_samples",
                "shadow_samples",
                "promotions",
                "filter_resets",
            )
        },
    }
    last_block = blocks[-1]
    decision = last_block.get("shadow_decision")
    shadow_evidence = {
        "last_decision_code": decision,
        "last_decision": SHADOW_DECISIONS.get(
            decision, "unavailable or unknown decision"
        ),
        "validation_count_first": blocks[0].get("shadow_validation_count"),
        "validation_count_last": last_block.get("shadow_validation_count"),
        "last_training_eligibility": last_block.get("shadow_training_eligibility"),
        "last_validation_eligibility": last_block.get("shadow_validation_eligibility"),
        "last_coherence": last_block.get("shadow_coherence"),
        "last_mic_error_ratio": last_block.get("shadow_mic_error_ratio"),
        "last_foreground_error_ratio": last_block.get("shadow_foreground_error_ratio"),
        "observed_decisions": sorted(
            {
                SHADOW_DECISIONS.get(
                    block.get("shadow_decision"), "unavailable or unknown decision"
                )
                for block in blocks
            }
        ),
        "limitation": "Eligibility and ratios describe the candidate filter's recorded validation intervals. They do not score near-end voice preservation.",
    }
    reference_transport = {
        "last_receipt_age_ms": last_block.get("reference_receipt_age_ms"),
        "last_raw_packet_rms": last_block.get("raw_reference_rms"),
        "last_window_reference_rms": last_block.get("window_reference_rms"),
        "last_window_downmix_rms": last_block.get("window_downmix_rms"),
        "last_window_reference_seconds": last_block.get("window_reference_seconds"),
        "limitation": "Receipt age describes incoming packet activity. Underruns are separate timeline events. Raw packet and rolling levels can cover different intervals from the saved DSP-ready reference.",
    }
    report = {
        "schema": 1,
        "sample_rate": rate,
        "frames": frames,
        "duration_seconds": frames / rate,
        "signal_levels": {name: level(values) for name, values in signals.items()},
        "host_output": comparison(signals["mic"], signals["output"]),
        "reference_valid_frames": available,
        "reference_invalid_frames": frames - available,
        "reference_valid_segments": len(spans),
        "blocks": blocks,
        "windows": windows,
        "activity": {
            "rms_threshold_dbfs": threshold_dbfs,
            "window_seconds": 0.25,
            "signals": activity,
            "limitation": "The threshold describes these measured excerpts, does not control the DSP, and cannot identify whether a video was playing or which Windows endpoint carries it.",
        },
        "filter_evidence": filter_evidence,
        "shadow_evidence": shadow_evidence,
        "reference_transport": reference_transport,
        "reference_scope": "reference.wav contains the mono DSP-ready timeline input. Invalid reference spans are zero-filled; raw arriving stereo capture PCM is not present in this file.",
        "interpretation": "Observed level change includes wanted speech. No clean near-end or echo stems are available, so ERLE and voice preservation cannot be scored.",
    }
    if probe_lag and available == frames:
        report["lag_probe"] = lag_probe(signals["mic"], signals["reference"], rate)
    elif probe_lag:
        report["lag_probe"] = {
            "status": "reference_interrupted",
            "limitation": "Full-recording search is withheld because missing reference samples can create misleading correlations; inspect valid spans or native replay.",
        }
    if runner:
        report["native_replay"] = native_replay(
            runner.resolve(),
            rate,
            signals["mic"],
            signals["reference"],
            signals["output"],
            spans,
        )
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument(
        "--runner", type=Path, help="optional local buncancel-offline executable"
    )
    parser.add_argument(
        "--no-lag-probe",
        action="store_true",
        help="skip the exploratory signed lag search",
    )
    parser.add_argument(
        "--blocks",
        action="store_true",
        help="include every sanitized timeline block in JSON output",
    )
    args = parser.parse_args()
    try:
        report = analyze(args.capture, args.runner, not args.no_lag_probe)
        if not args.blocks:
            del report["blocks"]
        print(json.dumps(report, indent=2, allow_nan=False))
        return 0
    except (OSError, ValueError, struct.error, subprocess.SubprocessError) as exc:
        parser.exit(1, f"local capture analysis failed: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
