"""Local diagnostic analysis distinguishes availability, quiet audio and changes."""

import importlib.util
import json
import math
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "analyze_capture",
    Path(__file__).resolve().parents[2] / "tools" / "analyze_capture.py",
)
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


class CaptureAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.rate = 8000
        self.frames = self.rate
        self.document = {
            "schema_version": 1,
            "sample_rate": self.rate,
            "captured_frames": self.frames,
            "complete": True,
            "blocks": [
                {
                    "frame_offset": 0,
                    "frames": self.frames,
                    "reference_usable": True,
                    "time_ms": 0,
                    "reference_time_ms": -20,
                    "reset": False,
                    "live_confidence": 0.747,
                    "adapted_samples": 4800,
                    "promotions": 0,
                }
            ],
        }
        rng = random.Random(311)
        self.reference = [rng.uniform(-0.02, 0.02) for _ in range(self.frames)]
        self.mic = [0.0] * 272 + [0.7 * sample for sample in self.reference[:-272]]
        self.output = [0.4 * sample for sample in self.mic]
        self.write()

    def write(self):
        for name, values in (
            ("reference.wav", self.reference),
            ("mic_input.wav", self.mic),
            ("processed.wav", self.output),
        ):
            analyzer.write_float_wav(self.root / name, self.rate, values)
        (self.root / "timeline.json").write_text(json.dumps(self.document))

    def analyze(self, probe=False):
        return analyzer.analyze(self.root, probe_lag=probe)

    def test_observed_attenuation_is_not_labelled_erle(self):
        report = self.analyze()
        self.assertGreater(report["host_output"]["changed_frames"], 7000)
        self.assertAlmostEqual(
            report["host_output"]["observed_mic_output_level_change_db"],
            20 * math.log10(2.5),
            places=5,
        )
        self.assertEqual(report["reference_invalid_frames"], 0)
        self.assertNotIn("erle", report["host_output"])
        self.assertIn("No clean near-end", report["interpretation"])
        self.assertEqual(report["blocks"][0]["live_confidence"], 0.747)
        self.assertEqual(report["blocks"][0]["reference_time_ms"], -20)
        json.dumps(report, allow_nan=False)

    def test_known_lag_is_recovered_without_modifying_signal(self):
        report = self.analyze(probe=True)
        self.assertEqual(report["lag_probe"]["status"], "exploratory")
        for window in report["lag_probe"]["windows"]:
            self.assertEqual(window["lag_samples"], 272)
            self.assertGreater(window["absolute_correlation"], 0.999)

    def test_screenshot_level_noise_stays_explicitly_quiet_and_dry(self):
        rng = random.Random(712)
        self.reference = [
            rng.uniform(-1, 1) * math.sqrt(3) * 10 ** (-74.7 / 20)
            for _ in range(self.frames)
        ]
        self.mic = [
            rng.uniform(-1, 1) * math.sqrt(3) * 10 ** (-77.9 / 20)
            for _ in range(self.frames)
        ]
        self.output = self.mic.copy()
        self.write()
        report = self.analyze()
        self.assertEqual(report["host_output"]["changed_frames"], 0)
        self.assertAlmostEqual(
            report["signal_levels"]["mic"]["rms_dbfs"], -77.9, delta=0.2
        )
        self.assertAlmostEqual(
            report["signal_levels"]["reference"]["rms_dbfs"], -74.7, delta=0.2
        )
        self.assertFalse(report["signal_levels"]["reference"]["exact_silence"])
        self.assertEqual(
            report["activity"]["signals"]["mic"]["windows_above_threshold"], 0
        )
        self.assertEqual(
            report["activity"]["signals"]["reference"]["windows_above_threshold"], 0
        )

    def test_interruption_withholds_full_recording_lag_claim(self):
        self.document["blocks"] = [
            {"frame_offset": 0, "frames": 3000, "reference_usable": True},
            {"frame_offset": 3000, "frames": 1000, "reference_usable": False},
            {
                "frame_offset": 4000,
                "frames": 4000,
                "reference_usable": True,
                "reset": True,
            },
        ]
        self.write()
        report = self.analyze(probe=True)
        self.assertEqual(report["reference_valid_frames"], 7000)
        self.assertEqual(report["reference_invalid_frames"], 1000)
        self.assertEqual(report["reference_valid_segments"], 2)
        self.assertEqual(report["lag_probe"]["status"], "reference_interrupted")

    def test_shadow_validation_and_reference_transport_are_summarized(self):
        self.document["blocks"][0].update(
            shadow_decision=7,
            shadow_validation_count=4,
            shadow_training_eligibility=0.9,
            shadow_validation_eligibility=1.0,
            shadow_coherence=0.7,
            shadow_mic_error_ratio=0.4,
            shadow_foreground_error_ratio=1.1,
            reference_receipt_age_ms=3.2,
            window_reference_rms=0.01,
            window_downmix_rms=0.008,
            window_reference_seconds=1.9,
        )
        self.write()
        report = self.analyze()
        evidence = report["shadow_evidence"]
        self.assertEqual(
            evidence["last_decision"],
            "current output already predicts the microphone better",
        )
        self.assertEqual(evidence["validation_count_last"], 4)
        self.assertEqual(evidence["last_foreground_error_ratio"], 1.1)
        self.assertEqual(report["reference_transport"]["last_receipt_age_ms"], 3.2)
        self.assertEqual(
            report["reference_transport"]["last_window_reference_seconds"], 1.9
        )
        self.assertEqual(
            report["filter_evidence"]["first_observed_adapted_samples"], 4800
        )
        self.assertNotIn("capture_began_with_trained_filter", report["filter_evidence"])
        self.assertIn(
            "cannot establish", report["filter_evidence"]["initial_state_limitation"]
        )

    def test_cli_hides_individual_blocks_unless_requested(self):
        script = Path(__file__).resolve().parents[2] / "tools" / "analyze_capture.py"
        for flag in ([], ["--blocks"]):
            process = subprocess.run(
                [sys.executable, str(script), str(self.root), "--no-lag-probe", *flag],
                capture_output=True,
                text=True,
                check=True,
            )
            report = json.loads(process.stdout)
            self.assertEqual("blocks" in report, bool(flag))
            self.assertIn("windows", report)
            self.assertIn("shadow_evidence", report)

    def test_pcm16_comparison_matches_native_rounding_and_clipping(self):
        self.assertEqual(
            analyzer.pcm16([0, 0.5 / 32768, -0.5 / 32768, 2, -2]),
            [0, 1 / 32768, -1 / 32768, 32767 / 32768, -1],
        )

    def test_reset_splits_otherwise_contiguous_reference_spans(self):
        blocks = [
            {"frame_offset": 0, "frames": 1000, "reference_usable": True},
            {
                "frame_offset": 1000,
                "frames": 1000,
                "reference_usable": True,
                "reset": True,
            },
            {"frame_offset": 2000, "frames": 1000, "reference_usable": True},
        ]
        self.assertEqual(analyzer.valid_spans(blocks), [(0, 1000), (1000, 3000)])

    def test_incomplete_or_overlapping_timeline_is_rejected(self):
        for start, count in (
            (1, self.frames),
            (0, self.frames - 1),
            (0, self.frames + 1),
        ):
            with self.subTest(start=start, count=count):
                self.document["blocks"][0].update(frame_offset=start, frames=count)
                self.write()
                with self.assertRaises(analyzer.CaptureError):
                    self.analyze()

    def test_report_does_not_copy_unknown_metadata_or_private_identifiers(self):
        self.document["unrecognized_path"] = "/private/recording/location"
        self.document["blocks"][0]["endpoint_id"] = "private-device-id"
        self.write()
        report = json.dumps(self.analyze())
        self.assertNotIn("private-device-id", report)
        self.assertNotIn("/private", report)

    def test_nonfinite_audio_or_metadata_and_wrong_wave_dimensions_are_rejected(self):
        self.document["blocks"][0]["live_confidence"] = float("nan")
        self.write()
        with self.assertRaises(analyzer.CaptureError):
            self.analyze()
        self.document["blocks"][0]["live_confidence"] = 0.7
        self.mic[0] = float("nan")
        self.write()
        with self.assertRaises(analyzer.CaptureError):
            self.analyze()
        self.mic = [0.0] * (self.frames - 1)
        self.write()
        with self.assertRaises(analyzer.CaptureError):
            self.analyze()


if __name__ == "__main__":
    unittest.main()
