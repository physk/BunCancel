---
name: audio-fixtures
description: Generate and validate deterministic acoustic echo cancellation WAV fixtures with explicit clean, reference and echo stems and media provenance.
---

# Audio fixtures skill

Trigger when creating, changing or scoring audio test signals or datasets.

1. Read docs/TEST_PLAN.md and the fixture generator contract.
2. Use a fixed seed and document all sample rates, channels, timestamps, delays, gains, acoustic path taps and nonlinear stages.
3. Emit reference.wav, clean_voice.wav, echo_only.wav, mic_input.wav and manifest.json per scenario.
4. Validate determinism using SHA-256 across two clean regenerations.
5. Never confuse generated synthetic speech-like sounds with licensed human speech.
6. Separate development fixtures from an untouched evaluation split. Document audio provenance and license before any non-synthetic sample is added.
7. Keep private conversations and identifying audio completely out of repository history and CI artifacts.
8. Add objective checks for scenario properties, not invented DSP results.
