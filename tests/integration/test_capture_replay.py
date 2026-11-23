"""Exercise local diagnostic analysis with the actual native offline runner."""

import importlib.util
import json
import math
import random
import sys
import tempfile
from pathlib import Path

MODULE = Path(__file__).resolve().parents[2] / "tools" / "analyze_capture.py"
SPEC = importlib.util.spec_from_file_location("analyze_capture", MODULE)
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


def main():
    runner = Path(sys.argv[1]).resolve()
    rate = 8000
    frames = rate * 3
    rng = random.Random(912)
    reference = [rng.uniform(-0.1, 0.1) for _ in range(frames)]
    mic = [0.0] * 219 + [0.7 * sample for sample in reference[:-219]]
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for name, samples in (
            ("reference.wav", reference),
            ("mic_input.wav", mic),
            ("processed.wav", mic),
        ):
            analyzer.write_float_wav(root / name, rate, samples)
        metadata = {
            "schema_version": 1,
            "complete": True,
            "sample_rate": rate,
            "captured_frames": frames,
            "blocks": [
                {
                    "frame_offset": 0,
                    "frames": frames,
                    "reference_usable": True,
                    "reset": True,
                }
            ],
        }
        (root / "timeline.json").write_text(json.dumps(metadata))
        report = analyzer.analyze(root, runner, probe_lag=False)
        result = report["native_replay"]["segments"][0]
        assert result["aligned"], "native replay did not acquire correlated echo"
        assert result["delay_samples"] == 219, "native replay did not recover input lag"
        assert (
            result["replay_vs_pcm16_dry"]["changed_frames"] > rate
        ), "native replay never changed microphone samples"
        assert (
            result["replay_vs_pcm16_dry"]["observed_mic_output_level_change_db"] > 3
        ), "native replay did not reduce simulated far-only signal"
        assert (
            report["host_output"]["changed_frames"] == 0
        ), "analysis confused actual captured host output with replay"
        assert (
            len(list(root.iterdir())) == 4
        ), "analysis persisted replay audio inside diagnostic folder"
        json.dumps(report, allow_nan=False)
        # A dry independent microphone must also compare exactly after the
        # native runner's PCM16 conversion; quantization is not subtraction.
        near = [rng.uniform(-0.1, 0.1) for _ in range(frames)]
        for name in ("mic_input.wav", "processed.wav"):
            analyzer.write_float_wav(root / name, rate, near)
        independent = analyzer.analyze(root, runner, probe_lag=False)
        segment = independent["native_replay"]["segments"][0]
        assert not segment["aligned"], "native replay aligned independent near speech"
        assert (
            segment["replay_vs_pcm16_dry"]["changed_frames"] == 0
        ), "PCM16 conversion was confused with subtraction"
    print(
        "Local analysis and native replay integration passed on synthetic correlated playback."
    )


if __name__ == "__main__":
    main()
