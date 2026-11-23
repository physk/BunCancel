#!/usr/bin/env python3
"""Generate the development corpus and run paired dry/processed scoring."""

import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    root = args.out.resolve()
    subprocess.run(
        [sys.executable, ROOT / "tests/audio/generate_fixtures.py", "--out", root],
        check=True,
    )
    results = {}
    for manifest in json.loads((root / "index.json").read_text()):
        name = manifest["scenario"]
        folder = root / name
        mic = folder / "mic_input.wav"
        processed = folder / "processed.wav"
        diagnostics = run(
            args.runner.resolve(), folder / "reference.wav", mic, processed
        )
        item = {"runner": json.loads(diagnostics)}
        for label, wav in (("dry", mic), ("processed", processed)):
            item[label] = json.loads(
                run(
                    sys.executable,
                    ROOT / "tools/score.py",
                    "--fixture",
                    folder,
                    "--processed",
                    wav,
                    "--latency-samples",
                    "0",
                )
            )
        results[name] = item
        print(f"Scored {name}", flush=True)
    (root / "metrics.json").write_text(
        json.dumps(results, indent=2, allow_nan=False) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
