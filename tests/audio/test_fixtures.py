"""Fixture integrity and repeatability, never AEC performance certification."""

import hashlib
import importlib.util
import json
import math
import random
import struct
import tempfile
import unittest
import wave
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("generate_fixtures.py")
SPEC = importlib.util.spec_from_file_location("generate_fixtures", MODULE_PATH)
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


def hashes(folder: Path) -> dict[str, str]:
    return {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(folder.iterdir())
        if p.is_file()
    }


def samples(path: Path) -> list[float]:
    with wave.open(str(path), "rb") as wav:
        data = wav.readframes(wav.getnframes())
    return [
        value / 32767 for value in struct.unpack("<" + "h" * (len(data) // 2), data)
    ]


class FixtureTests(unittest.TestCase):
    def test_all_scenarios_have_correct_files_and_shapes(self):
        with tempfile.TemporaryDirectory() as temp:
            for scenario in generator.SCENARIOS:
                root = Path(temp) / scenario
                manifest = generator.generate_scenario(
                    scenario, root, sr=8000, seconds=0.35
                )
                self.assertTrue(manifest["synthetic_only"])
                self.assertEqual(manifest["dataset_split"], "development")
                self.assertEqual(manifest["frame_count"], 2800)
                self.assertEqual(manifest["schema"], 2)
                self.assertEqual(
                    manifest["provenance"]["generator_sha256"],
                    hashlib.sha256(MODULE_PATH.read_bytes()).hexdigest(),
                )
                for filename, channels in manifest["channels"].items():
                    with wave.open(str(root / filename), "rb") as wav:
                        self.assertEqual(wav.getnchannels(), channels)
                        self.assertEqual(wav.getsampwidth(), 2)
                        self.assertEqual(wav.getframerate(), 8000)
                        self.assertEqual(wav.getnframes(), 2800)
                    self.assertEqual(
                        manifest["wav_sha256"][filename], hashes(root)[filename]
                    )
                self.assertEqual(
                    json.loads((root / "manifest.json").read_text()), manifest
                )

    def test_reproducible_bytes_for_every_scenario(self):
        with tempfile.TemporaryDirectory() as temp:
            for scenario in generator.SCENARIOS:
                first, second = Path(temp) / "first", Path(temp) / "second"
                generator.generate_scenario(
                    scenario, first, sr=8000, seconds=0.35, seed=900
                )
                generator.generate_scenario(
                    scenario, second, sr=8000, seconds=0.35, seed=900
                )
                self.assertEqual(hashes(first), hashes(second), scenario)

    def test_reference_silent_and_baseline_voice_silent(self):
        with tempfile.TemporaryDirectory() as temp:
            for scenario, filename in (
                ("reference-silent", "reference.wav"),
                ("baseline", "clean_voice.wav"),
            ):
                folder = Path(temp) / scenario
                generator.generate_scenario(scenario, folder, sr=8000, seconds=0.35)
                self.assertTrue(all(x == 0 for x in samples(folder / filename)))

    def test_windows_match_stem_activity_and_echo_tail(self):
        with tempfile.TemporaryDirectory() as temp:
            for scenario in generator.SCENARIOS:
                folder = Path(temp) / scenario
                manifest = generator.generate_scenario(
                    scenario, folder, sr=8000, seconds=1.5
                )
                near = samples(folder / "clean_voice.wav")
                echo = samples(folder / "echo_only.wav")
                kinds = set()
                for window in manifest["evaluation_windows"]:
                    start, end = window["start_sample"], window["end_sample"]
                    self.assertTrue(0 <= start < end <= len(near))
                    kinds.add(window["kind"])
                    near_energy = sum(x * x for x in near[start:end])
                    echo_energy = sum(x * x for x in echo[start:end])
                    if window["kind"] == "far_only":
                        self.assertEqual(near_energy, 0)
                        self.assertGreater(echo_energy, 0.001)
                    elif window["kind"] == "near_only":
                        self.assertEqual(echo_energy, 0)
                        self.assertGreater(near_energy, 0.001)
                    else:
                        self.assertEqual(window["kind"], "double_talk")
                        self.assertGreater(near_energy, 0.001)
                        self.assertGreater(echo_energy, 0.001)
                expected = (
                    {"near_only"} if scenario == "reference-silent" else {"far_only"}
                )
                if scenario not in ("baseline", "reference-silent"):
                    expected.add("double_talk")
                    if scenario != "long-delay":
                        expected.add("near_only")
                self.assertTrue(expected <= kinds, scenario)

    def test_echo_matches_declared_delay_drift_gain_and_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            for scenario in generator.SCENARIOS:
                folder = Path(temp) / scenario
                manifest = generator.generate_scenario(
                    scenario, folder, sr=8000, seconds=0.8
                )
                stereo = samples(folder / "reference.wav")
                render = [
                    0.64 * left + 0.36 * right
                    for left, right in zip(stereo[::2], stereo[1::2])
                ]
                actual = samples(folder / "echo_only.wav")
                base_delay = manifest["reference_to_mic_base_delay_samples"]
                self.assertTrue(all(value == 0 for value in actual[:base_delay]))
                largest_error = 0.0
                for i, value in enumerate(actual):
                    taps = manifest["path_taps"]
                    change = manifest["path_change"]
                    if change and i >= change["sample"]:
                        taps = change["path_taps"]
                    gain = manifest["acoustic_gain"]
                    change = manifest["gain_change"]
                    if change and i >= change["sample"]:
                        gain *= change["multiplier"]
                    expected = 0.0
                    for tap in taps:
                        source = (
                            i
                            - base_delay
                            - tap["extra_delay_samples"]
                            - i * manifest["drift_ppm"] / 1_000_000
                        )
                        lo = math.floor(source)
                        fraction = source - lo
                        a = render[lo] if 0 <= lo < len(render) else 0
                        b = render[lo + 1] if 0 <= lo + 1 < len(render) else 0
                        expected += tap["gain"] * (a * (1 - fraction) + b * fraction)
                    largest_error = max(largest_error, abs(value - expected * gain))
                # Reference and echo are independently quantized to 16-bit PCM.
                self.assertLess(largest_error, 2 / 32767, scenario)
                if scenario == "drift":
                    self.assertGreater(manifest["drift_samples_end"], 3)
                if scenario == "path-change":
                    self.assertNotEqual(
                        manifest["path_taps"], manifest["path_change"]["path_taps"]
                    )

    def test_scenarios_exercise_distinct_physical_challenges(self):
        with tempfile.TemporaryDirectory() as temp:
            manifests = {}
            echoes = {}
            for scenario in (
                "baseline",
                "headphones-on-mic",
                "long-delay",
                "drift",
                "gain-change",
            ):
                folder = Path(temp) / scenario
                manifests[scenario] = generator.generate_scenario(
                    scenario, folder, sr=8000, seconds=0.8
                )
                echoes[scenario] = samples(folder / "echo_only.wav")
            self.assertEqual(
                manifests["long-delay"]["reference_to_mic_base_delay_samples"], 1920
            )
            self.assertLess(
                manifests["headphones-on-mic"]["reference_to_mic_base_delay_samples"],
                80,
            )
            self.assertGreater(
                sum(x * x for x in echoes["headphones-on-mic"]),
                sum(x * x for x in echoes["baseline"]),
            )
            self.assertAlmostEqual(manifests["drift"]["drift_ppm"], 533.3333333333334)
            self.assertEqual(
                manifests["gain-change"]["gain_change"],
                {"sample": 3200, "multiplier": 1.65},
            )

    def test_microphone_mixture_noise_and_clipping(self):
        with tempfile.TemporaryDirectory() as temp:
            for scenario in generator.SCENARIOS:
                folder = Path(temp) / scenario
                manifest = generator.generate_scenario(
                    scenario, folder, sr=8000, seconds=0.8
                )
                near = samples(folder / "clean_voice.wav")
                echo = samples(folder / "echo_only.wav")
                mic = samples(folder / "mic_input.wav")
                processing = manifest["microphone_processing"]
                rng = random.Random(manifest["noise"]["seed"])
                errors = []
                for voice, leakage, value in zip(near, echo, mic):
                    noise = rng.uniform(-0.0005, 0.0005)
                    summed = (voice + leakage + noise) * processing["preamp"]
                    expected = min(1.0, max(-1.0, summed)) * processing["output_gain"]
                    errors.append(abs(value - expected))
                self.assertLess(max(errors), (processing["preamp"] + 1) / 32767)
                if scenario == "nonlinear-clip":
                    self.assertGreater(processing["clipped_sample_count"], 0)
                    ceiling = round(processing["output_gain"] * 32767) / 32767
                    self.assertTrue(any(abs(value) == ceiling for value in mic))
                else:
                    self.assertEqual(processing["clipped_sample_count"], 0)

    def test_invalid_parameters_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            for sr, duration in (
                (7999, 1),
                (8000.5, 1),
                (8000, 0),
                (8000, -1),
                (8000, float("nan")),
                (8000, float("inf")),
                (8000, 0.000001),
            ):
                with self.assertRaises(ValueError):
                    generator.generate_scenario("baseline", Path(temp), sr, duration)
            with self.assertRaises(ValueError):
                generator.generate_scenario("unknown", Path(temp))


if __name__ == "__main__":
    unittest.main()
