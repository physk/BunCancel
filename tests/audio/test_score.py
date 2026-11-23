"""Deterministic scoring-contract regressions, independent of AEC tuning."""

import importlib.util
import json
import math
import struct
import tempfile
import unittest
import wave
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "score", Path(__file__).resolve().parents[2] / "tools" / "score.py"
)
scorer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scorer)


class ScoreTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.clean = [0] * 8 + [1000, -1000] * 8
        self.echo = [500, -500] * 4 + [0] * 8 + [500, -500] * 4
        self.mic = [a + b for a, b in zip(self.clean, self.echo)]
        self.reference = [v for sample in self.echo for v in (sample, sample)]
        self.manifest = {
            "schema": 2,
            "scenario": "unit-test",
            "sample_rate_hz": 8000,
            "frame_count": 24,
            "evaluation_windows": [
                {"kind": "far_only", "start_sample": 0, "end_sample": 8},
                {"kind": "near_only", "start_sample": 8, "end_sample": 16},
                {"kind": "double_talk", "start_sample": 16, "end_sample": 24},
            ],
        }
        self.write_manifest()
        self.write("reference.wav", self.reference, 2)
        self.write("clean_voice.wav", self.clean)
        self.write("echo_only.wav", self.echo)
        self.write("mic_input.wav", self.mic)
        self.write("processed.wav", self.mic)

    def write_manifest(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest))

    def write(self, name, samples, channels=1, rate=8000):
        with wave.open(str(self.root / name), "wb") as wav:
            wav.setnchannels(channels)
            wav.setsampwidth(2)
            wav.setframerate(rate)
            wav.writeframes(struct.pack(f"<{len(samples)}h", *samples))

    def score(self, latency=0):
        return scorer.score(self.root, self.root / "processed.wav", latency)

    def test_dry_baseline_is_zero_erle_and_has_separate_near_metrics(self):
        metrics = self.score()["metrics"]
        self.assertEqual(metrics["far_only"]["erle"], {"db": 0.0, "status": "finite"})
        self.assertEqual(metrics["near_only"]["processed"]["gain"], 1.0)
        self.assertEqual(metrics["near_only"]["processed"]["snr"]["status"], "perfect")
        self.assertEqual(metrics["double_talk"]["processed"]["gain"], 1.5)
        self.assertNotIn("erle", metrics["double_talk"])
        self.assertAlmostEqual(
            metrics["double_talk"]["processed"]["snr"]["db"], 20 * math.log10(2)
        )

    def test_half_gain_attenuation_cannot_hide_near_damage(self):
        self.write("processed.wav", [value // 2 for value in self.mic])
        metrics = self.score()["metrics"]
        self.assertAlmostEqual(metrics["far_only"]["erle"]["db"], 20 * math.log10(2))
        self.assertEqual(metrics["near_only"]["processed"]["gain"], 0.5)
        self.assertAlmostEqual(
            metrics["near_only"]["processed"]["snr"]["db"], 20 * math.log10(2)
        )
        self.assertEqual(
            metrics["near_only"]["processed"]["si_sdr"]["status"], "perfect"
        )

    def test_explicit_latency_alignment_requires_flushed_tail(self):
        self.write("processed.wav", [0, 0, 0] + self.mic)
        self.assertEqual(self.score(3)["metrics"]["far_only"]["erle"]["db"], 0)
        with self.assertRaises(scorer.ScoreError):
            self.score(0)
        self.write("processed.wav", [0, 0, 0] + self.mic[:-3])
        with self.assertRaises(scorer.ScoreError):
            self.score(3)

    def test_silent_output_is_explicit_and_json_finite(self):
        self.write("processed.wav", [0] * 24)
        report = self.score()
        self.assertEqual(report["metrics"]["far_only"]["erle"]["status"], "perfect")
        self.assertEqual(report["metrics"]["near_only"]["processed"]["gain"], 0)
        self.assertEqual(report["metrics"]["near_only"]["processed"]["snr"]["db"], 0)
        self.assertEqual(
            report["metrics"]["near_only"]["processed"]["si_sdr"]["status"],
            "silent_output",
        )
        json.dumps(report, allow_nan=False)

    def test_rejects_wrong_rate_channels_and_frame_count(self):
        for samples, channels, rate in [
            (self.mic, 1, 16000),
            (self.mic * 2, 2, 8000),
            (self.mic[:-1], 1, 8000),
        ]:
            with self.subTest(channels=channels, rate=rate, length=len(samples)):
                self.write("processed.wav", samples, channels, rate)
                with self.assertRaises(scorer.ScoreError):
                    self.score()

    def test_rejects_invalid_windows_and_negative_latency(self):
        with self.assertRaises(scorer.ScoreError):
            self.score(-1)
        for window in [
            {"kind": "far_only", "start_sample": 8, "end_sample": 16},
            {"kind": "near_only", "start_sample": 0, "end_sample": 8},
            {"kind": "double_talk", "start_sample": 8, "end_sample": 16},
            {"kind": "far_only", "start_sample": 0, "end_sample": 25},
            {"kind": "far_only", "start_sample": 0, "end_sample": 0},
            {"kind": "far_only", "start_sample": False, "end_sample": 8},
            {"kind": "typo", "start_sample": 0, "end_sample": 8},
        ]:
            with self.subTest(window=window):
                self.manifest["evaluation_windows"] = [window]
                self.write_manifest()
                with self.assertRaises(scorer.ScoreError):
                    self.score()

    def test_rejects_overlapping_windows(self):
        self.manifest["evaluation_windows"] *= 2
        self.write_manifest()
        with self.assertRaises(scorer.ScoreError):
            self.score()

    def test_clipping_and_modeled_nonlinearity_disable_erle(self):
        self.manifest["nonlinear_mic_clip"] = True
        self.write_manifest()
        self.assertEqual(
            self.score()["metrics"]["far_only"]["erle"]["status"],
            "invalid_nonlinear_mic",
        )
        self.manifest["nonlinear_mic_clip"] = False
        self.write_manifest()
        self.mic[0] = 32767
        self.write("mic_input.wav", self.mic)
        self.write("processed.wav", [-32768] + self.mic[1:])
        metrics = self.score()["metrics"]["far_only"]
        self.assertEqual(metrics["dry_clip_samples"], 1)
        self.assertEqual(metrics["output_clip_samples"], 1)
        self.assertEqual(metrics["erle"]["status"], "invalid_nonlinear_mic")

    def test_absent_windows_and_silent_targets_have_no_fabricated_metrics(self):
        self.manifest["evaluation_windows"] = []
        self.write_manifest()
        self.assertEqual(
            self.score()["metrics"]["far_only"],
            {"sample_count": 0, "status": "no_windows"},
        )
        self.assertEqual(
            scorer.fidelity([0.0], [1.0])["snr"]["status"], "silent_target"
        )

    def test_requires_schema_two(self):
        self.manifest["schema"] = 1
        self.write_manifest()
        with self.assertRaises(scorer.ScoreError):
            self.score()


if __name__ == "__main__":
    unittest.main()
