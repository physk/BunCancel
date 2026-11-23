---
name: aec-dsp
description: Design and benchmark original acoustic echo cancellation DSP, including delay estimation, adaptive filtering, residual suppression and double-talk safeguards.
---

# AEC DSP skill

Trigger when working on the portable cancellation engine or evaluating changes to it.

1. Read docs/ARCHITECTURE.md and docs/TEST_PLAN.md.
2. Explain proposed algorithm, signal model and invariants without copying third-party AEC code.
3. Build an offline reproducer and measure against unprocessed mic.
4. Report echo cancellation during far-end-only windows separately from near-end fidelity during double-talk.
5. Ensure adaptive coefficients remain stable during reference dropout, nonlinear overload and missing alignment.
6. Test known path length, changed gain, drift, long delay, stereo render and strongly contaminated mic.
7. Benchmark callback execution time, transient allocations, numerical stability and latency.
8. Report both successes and failures. A simpler filter that preserves speech is preferable to an aggressive filter that removes the speaker.
