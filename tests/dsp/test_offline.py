"""Portable offline CLI contract checks; pass the executable as the first argument."""

import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())


def wav_bytes(values, channels=1, rate=48000, bits=32, encoding=3):
    if encoding == 3:
        data = struct.pack("<" + "f" * len(values), *values)
    else:
        scale = 1 << (bits - 1)
        data = b"".join(
            int(value * scale).to_bytes(bits // 8, "little", signed=True)
            for value in values
        )
    alignment = channels * bits // 8
    fmt = struct.pack("<HHIIHH", encoding, channels, rate, rate * alignment,
                      alignment, bits)
    body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"data" + struct.pack("<I", len(data)) + data
    if len(data) % 2:
        body += b"\0"
    return b"RIFF" + struct.pack("<I", len(body)) + body


class OfflineTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.reference = self.root / "reference.wav"
        self.mic = self.root / "mic.wav"
        self.output = self.root / "processed.wav"
        self.reference.write_bytes(wav_bytes([0.0] * 513))
        self.values = [math.sin(i * 0.17) * 0.25 for i in range(513)]
        self.mic.write_bytes(wav_bytes(self.values))

    def run_cli(self, block="64"):
        return subprocess.run(
            [EXECUTABLE, str(self.reference), str(self.mic), str(self.output), block],
            capture_output=True, text=True, check=False,
        )

    def test_float_roundtrip_and_partial_block(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        metrics = json.loads(result.stdout)
        self.assertEqual(metrics["frames"], 513)
        self.assertEqual(metrics["sample_rate"], 48000)
        self.assertIsNone(metrics["erle_db"])
        self.assertIsNone(metrics["near_end_fidelity"])
        self.assertGreaterEqual(metrics["process_max_us"], metrics["process_p99_us"])
        self.assertGreaterEqual(metrics["process_p99_us"], metrics["process_p50_us"])
        self.assertGreaterEqual(metrics["process_total_us"], metrics["process_max_us"])
        payload = self.output.read_bytes()
        self.assertEqual(payload[0:4], b"RIFF")
        self.assertEqual(struct.unpack_from("<I", payload, 4)[0], len(payload) - 8)
        self.assertEqual(struct.unpack_from("<HH", payload, 20), (1, 1))
        decoded = struct.unpack_from("<513h", payload, 44)
        expected = tuple(round(value * 32768) for value in self.values)
        self.assertEqual(decoded, expected)
        # The emitted PCM16 WAV can itself be consumed without further quantization.
        self.mic.write_bytes(payload)
        second = self.run_cli("127")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.output.read_bytes(), payload)

    def test_pcm_formats_and_stereo_mean(self):
        for bits in (16, 24, 32):
            with self.subTest(bits=bits):
                samples = [0.5, -0.25] * 513
                self.mic.write_bytes(wav_bytes(samples, channels=2, bits=bits, encoding=1))
                result = self.run_cli()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)["mic_channels"], 2)
                decoded = struct.unpack_from("<513h", self.output.read_bytes(), 44)
                self.assertEqual(decoded, (4096,) * 513)

    def test_stereo_reference_downmix(self):
        # Anti-phase stereo must become a silent reference under the documented mean policy.
        self.reference.write_bytes(wav_bytes([0.5, -0.5] * 513, channels=2))
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        metrics = json.loads(result.stdout)
        self.assertEqual(metrics["reference_channels"], 2)
        self.assertFalse(metrics["aligned"])
        self.assertEqual(struct.unpack_from("<513h", self.output.read_bytes(), 44),
                         tuple(round(value * 32768) for value in self.values))

    def test_pcm16_clipping_is_reported(self):
        self.mic.write_bytes(wav_bytes([1.5, -1.5] + [0.0] * 511))
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        metrics = json.loads(result.stdout)
        self.assertEqual(metrics["output_clipped_samples"], 2)
        self.assertEqual(struct.unpack_from("<2h", self.output.read_bytes(), 44),
                         (32767, -32768))
        self.assertGreater(metrics["dsp_output_rms"], metrics["output_rms"])

    def test_reject_malformed_and_unsupported(self):
        valid = wav_bytes(self.values)
        cases = [b"", valid[:11], valid[:-1], valid + b"extra",
                 wav_bytes([float("nan")]), wav_bytes([float("inf")]),
                 wav_bytes([0.0] * 513, channels=3),
                 wav_bytes([0.0] * 513, rate=44100),
                 wav_bytes([0.0] * 512)]
        damaged = bytearray(valid)
        struct.pack_into("<I", damaged, 40, 0xFFFFFFFF)
        cases.append(bytes(damaged))
        damaged = bytearray(valid)
        struct.pack_into("<H", damaged, 32, 1)
        cases.append(bytes(damaged))
        damaged = bytearray(valid)
        struct.pack_into("<H", damaged, 20, 6)
        cases.append(bytes(damaged))
        for payload in cases:
            with self.subTest(size=len(payload)):
                self.mic.write_bytes(payload)
                result = self.run_cli()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("buncancel-offline:", result.stderr)
                self.assertFalse(self.output.exists())

    def test_block_validation(self):
        for block in ("0", "-1", "65537", "64bad", "99999999999999999"):
            with self.subTest(block=block):
                self.assertNotEqual(self.run_cli(block).returncode, 0)


if __name__ == "__main__":
    unittest.main()
