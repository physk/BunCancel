#!/usr/bin/env python3
"""Optional independent offline comparator for the generated PCM16 development corpus."""

import argparse
import ctypes as ct
from ctypes.util import find_library
import json
from pathlib import Path
import struct
import subprocess
import sys
import wave


def load_library():
    name = find_library("speexdsp")
    if not name:
        raise SystemExit(
            "Install the system SpeexDSP shared library for this comparison"
        )
    lib = ct.CDLL(name)
    sample_ptr = ct.POINTER(ct.c_int16)
    lib.speex_echo_state_init.argtypes = [ct.c_int, ct.c_int]
    lib.speex_echo_state_init.restype = ct.c_void_p
    lib.speex_echo_ctl.argtypes = [ct.c_void_p, ct.c_int, ct.c_void_p]
    lib.speex_echo_cancellation.argtypes = [
        ct.c_void_p,
        sample_ptr,
        sample_ptr,
        sample_ptr,
    ]
    lib.speex_echo_cancellation.restype = None
    lib.speex_echo_state_destroy.argtypes = [ct.c_void_p]
    lib.speex_echo_state_destroy.restype = None
    return lib


def compare(folder, lib):
    output = folder / "speex.wav"
    with wave.open(str(folder / "reference.wav")) as ref, wave.open(
        str(folder / "mic_input.wav")
    ) as mic:
        rate = mic.getframerate()
        block = rate // 100
        if (
            block < 1
            or mic.getnchannels() != 1
            or ref.getnchannels() != 2
            or mic.getsampwidth() != 2
            or ref.getsampwidth() != 2
            or ref.getframerate() != rate
            or mic.getnframes() != ref.getnframes()
        ):
            raise ValueError(
                "Expected equal-length, equal-rate PCM16 stereo ref and mono mic"
            )
        frame = ct.c_int16 * block
        state = lib.speex_echo_state_init(block, round(rate * 0.32))
        if not state:
            raise MemoryError("Unable to initialize independent comparator")
        try:
            # SPEEX_ECHO_SET_SAMPLING_RATE from speex_echo.h
            if lib.speex_echo_ctl(state, 24, ct.byref(ct.c_int(rate))) != 0:
                raise RuntimeError("SpeexDSP did not accept the sample rate")
            with wave.open(str(output), "wb") as dest:
                dest.setparams(mic.getparams())
                for start in range(0, mic.getnframes(), block):
                    count = min(block, mic.getnframes() - start)
                    captured = frame(
                        *struct.unpack(f"<{count}h", mic.readframes(count))
                    )
                    played = frame(
                        *(
                            round((a + b) / 2)
                            for a, b in struct.iter_unpack("<hh", ref.readframes(count))
                        )
                    )
                    result = frame()
                    lib.speex_echo_cancellation(state, captured, played, result)
                    dest.writeframesraw(struct.pack(f"<{count}h", *result[:count]))
        finally:
            lib.speex_echo_state_destroy(state)
    score = Path(__file__).with_name("score.py")
    return json.loads(
        subprocess.check_output(
            [
                sys.executable,
                score,
                "--fixture",
                folder,
                "--processed",
                output,
                "--latency-samples",
                "0",
            ],
            text=True,
        )
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path)
    args = parser.parse_args()
    lib = load_library()
    scenarios = json.loads((args.corpus / "index.json").read_text())
    result = {
        m["scenario"]: compare(args.corpus / m["scenario"], lib) for m in scenarios
    }
    (args.corpus / "speex-metrics.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
