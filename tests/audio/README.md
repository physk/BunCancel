# Synthetic audio fixtures

Generate eight deterministic acoustic echo scenarios using the standard library:

python tests/audio/generate_fixtures.py --out build/fixtures --seconds 3

Each scenario has stereo **reference.wav**, mono **clean_voice.wav**, **echo_only.wav**, **mic_input.wav**, and **manifest.json**. See docs/TEST_PLAN.md.

This tests infrastructure and declared signal roles only. The synthetic, harmonic test audio is **not natural speech**, a substitute for real data or evidence that BunCancel actually cancels echo. No DSP has been implemented at repository bootstrap.
