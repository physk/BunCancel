"""Only fixture integrity and repeatability; NOT AEC performance certification."""

import hashlib
import importlib.util
import json
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
        for p in sorted(folder.iterdir()) if p.is_file()
    }


class FixtureTests(unittest.TestCase):
    def test_all_scenarios_have_correct_files_and_shapes(self):
        with tempfile.TemporaryDirectory() as temp:
            for scenario in generator.SCENARIOS:
                root = Path(temp) / scenario
                manifest = generator.generate_scenario(scenario, root, sr=8000, seconds=0.35)
                self.assertTrue(manifest["synthetic_only"])
                self.assertEqual(manifest["frame_count"], 2800)
                for filename, channels in manifest["channels"].items():
                    with wave.open(str(root / filename), "rb") as wav:
                        self.assertEqual(wav.getnchannels(), channels)
                        self.assertEqual(wav.getsampwidth(), 2)
                        self.assertEqual(wav.getframerate(), 8000)
                        self.assertEqual(wav.getnframes(), 2800)
                self.assertEqual(json.loads((root / "manifest.json").read_text())["scenario"], scenario)

    def test_reproducible_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            first, second = Path(temp) / "first", Path(temp) / "second"
            generator.generate_scenario("double-talk", first, sr=8000, seconds=0.35, seed=900)
            generator.generate_scenario("double-talk", second, sr=8000, seconds=0.35, seed=900)
            self.assertEqual(hashes(first), hashes(second))

    def test_reference_silent_and_baseline_voice_silent(self):
        with tempfile.TemporaryDirectory() as temp:
            for scenario, filename in (("reference-silent", "reference.wav"), ("baseline", "clean_voice.wav")):
                folder = Path(temp) / scenario
                generator.generate_scenario(scenario, folder, sr=8000, seconds=0.35)
                with wave.open(str(folder / filename), "rb") as wav:
                    self.assertEqual(set(wav.readframes(wav.getnframes())), {0})


if __name__ == "__main__":
    unittest.main()
