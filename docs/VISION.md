# Product vision: this is what BunCancel should be

## The problem

During Discord calls, other people's voices play through headphones and leak back into the microphone. They hear themselves as delayed echo even though audio routing is correctly separated. The uncomfortable workaround is to reduce playback volume, move the headphones or microphone, or configure a gate aggressive enough to damage the wanted voice.

## The desired experience

Install a VST3 plugin on the microphone's effects chain, select **the actual mix being heard** as the reference — for example **Wave Link 3 Personal Mix** — and have the playback leakage disappear from what others hear. The far-end voice, music or game sound should be reduced wherever it acoustically reaches the microphone. The person using the microphone must still sound natural, including while talking at the same time as someone in chat.

The deliberately tough demonstration: put wireless headphones directly beside or on the microphone while voice chat plays, then speak normally. The desired result is an intelligible, natural near-end voice with the playback leakage reduced as far as physics and real-time compute allow. This is a **stress test and product goal**, not a promise of absolute silence.

## Product personality

Friendly and approachable rather than intimidating studio jargon. A cheerful pink-and-lavender headphone-wearing bunny represents the tool. Technical readouts remain accurate and available for diagnosis. Primary interface: one-click enable, device/reference picker, status and honest diagnostics; advanced settings only when useful.

Tagline: **Keep your voice. Lose the echo.**

## Principles

- Solve **acoustic** bleed, not misconfigured digital mixer loops.
- Use the full personal playback mix when available; avoid making users select ten apps separately.
- The microphone must not be inside the reference in a way that turns near-end speech into a cancellation target.
- Reference device selection should also work outside Wave Link: virtual mixers, conventional Windows speakers/headphones and future platforms.
- Never sacrifice ordinary voice usability to chase attractive echo-reduction numbers.
- Never require the original requester to sit through repeated manual checks. Generate deterministic test signals, ground truth, and automated scores first, then reserve a small number of physical tests for release qualification.
- Be open source, private by default and transparent about limitations.
- Use original algorithms built in this codebase; existing DSP engines are for independent benchmarking, not copying.

## Success looks like

A caller who hears echo from headphones through a microphone can install and route BunCancel, run a quick calibration, talk naturally while others are talking and stop generating distracting echo. Benchmarks, example files and supported-device limitations are publicly documented with no invented results.
