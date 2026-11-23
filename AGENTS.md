# Agent operating contract — BunCancel

This repository is the authoritative project specification. Act as an engineer working on a public open-source audio plugin, not as a chat participant.

## Mission

Deliver an original Windows-first, low-latency VST3 acoustic echo canceller. A microphone signal containing near-end speech and leaked playback must be processed using a clean, time-aligned playback reference to suppress the correlated echo **without cancelling near-end speech**. Start with deterministic test fixtures, then build the DSP, capture integration and plugin. A Wave Link 3 Personal Mix reference is the first target scenario; support ordinary Windows devices.

## Reading order

1. docs/VISION.md — product intent
2. docs/REQUIREMENTS.md — functional and nonfunctional commitments
3. docs/ARCHITECTURE.md — design boundaries and threading
4. docs/TEST_PLAN.md — measurement rules
5. docs/ROADMAP.md — milestones and acceptance gates

## Nonnegotiables

- **Do not insert a person's name, account identity or organization attribution into project documentation, authorship, comments, test data, metadata or UI.** Use neutral project language, e.g. "BunCancel should..." rather than "X wants...".
- **Do not fork or paste code from an existing AEC engine**. The AEC DSP must be project-authored. Standard libraries, VST3 SDK tooling, reference implementations used exclusively for independent comparisons and properly licensed non-AEC dependencies may be evaluated explicitly.
- **Do not pretend a passing build equals audio quality.** Report measured ERLE only in valid single-talk intervals, and verify voice retention in double-talk.
- The raw mic is the input. The reference is evidence of the contaminating playback, **not audio to add to the mic**.
- **Never train or tune against held-out evaluation fixtures.** Keep public test assets redistributable with provenance.
- No secrets, recordings of private voice calls, account handles or personal paths in tracked files.
- Defer features rather than risking dropouts, feedback, deadlocks, high CPU, broad device permissions or irreversible changes to mixer routing.
- Maintain a dry-mic path for reference loss; do not mute or pump the user's voice when playback is silent or capture disconnects.
- Inspect and preserve working code; use scoped changes and targeted tests before broad refactors.

## Workflow for each change

1. State the failure case and why the proposed change should help.
2. Add or select a deterministic reproducer; assert observable properties.
3. Implement the smallest useful increment. Keep DSP, OS audio capture and host integration independently testable.
4. Run relevant tests and measurable offline A/B evaluation. Track CPU time, allocation behavior and audio glitches where relevant.
5. Attach before/after WAV artifacts and a metrics summary for meaningful DSP changes. Record limitations and uncertain measurements.
6. Make the code easy to review; update documentation when behavior or requirements change.

## Audio-thread policy

No allocations, locks, blocking APIs, file access, logging with synchronous I/O, waits, or device controls in the VST3 process callback. Capture on a separate worker and exchange timestamped audio via bounded lock-free/SPSC queues with explicit overrun behavior. Declare and measure the plugin's processing latency. Audio clocks may drift: model both offset and rate error.

## Quality policy

Unit tests should prove internal invariants; integration tests must exercise buffer sizes, formats, device changes, UI reopen, shutdown, bypass and bypass transitions. AEC quality tests must separate echo attenuation from near-end damage. Compare against a measured baseline. Do not hardcode unachieved reduction or latency targets as passed.

## Skills

- .agents/skills/audio-fixtures/SKILL.md — fixture synthesis, provenance and corpus hygiene
- .agents/skills/aec-dsp/SKILL.md — delay alignment, adaptive estimation and double-talk measurements
- .agents/skills/windows-vst3/SKILL.md — real-time plugin and WASAPI integration
- .agents/skills/quality-release/SKILL.md — CI, benchmarks, packaging and acceptance gates

## Priority

Work in roadmap order unless blocking evidence justifies changing it. Begin with test infrastructure. No claims of real-world echo cancellation without a reproducible recording and an objective report.
