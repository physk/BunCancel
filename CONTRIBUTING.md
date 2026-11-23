# Contributing to BunCancel

Contributions are welcome, especially acoustics/DSP research, deterministic audio test fixtures, Windows audio device compatibility and VST3 host testing.

## Before changing DSP

Read AGENTS.md and the architecture/test plan. Open a focused issue describing the acoustic failure case, test conditions, intended metric, expected effect on near-end speech and possible real-time cost. Add a reproducible test before optimizing.

## Scope and code quality

- C++20; small independent modules; deterministic interfaces.
- Never allocate, block or use locks in the VST3 audio callback.
- Keep audio device capture, DSP and UI modular.
- Test bypass, reference silence, device unplug, clock drift and two people talking.
- Measure CPU and latency rather than substituting a convenient guess.
- Add fixture provenance and licensing information for all external media.

## Running initial tests

Requires Python 3.10+:

- python tests/audio/generate_fixtures.py --out build/fixtures
- python -m unittest discover -s tests/audio -p "test_*.py" -v

See README.md for the CMake, CTest and offline A/B commands. Report synthetic metrics separately from host and hardware acceptance.

## Attribution and content

Write documentation as project requirements, not personal requests or attributed quotes. Never include personal names, organizational identifiers, account details, private conversations or home-directory paths in tracked materials. Original or redistributable audio assets only.

## License

By contributing, you agree that your original contributions are available under the project MIT license. Third-party material keeps its own license and requires review.
