---
name: windows-vst3
description: Implement safe Windows WASAPI reference acquisition and VST3 host processing without blocking the real-time audio callback.
---

# Windows VST3 and capture skill

Trigger for plugin-host integration, Windows endpoint capture or host/device bug fixing.

1. Confirm device endpoint classes at runtime; Wave Link mix names do not imply WASAPI loopback accessibility.
2. Enumerate/select the reference via control thread, not process callback.
3. Run capture asynchronously; use timestamped bounded single-producer/single-consumer queue or proven equivalent.
4. Avoid allocations, waits, locks, API calls, logging I/O, UI and device setup in the audio callback.
5. Test mismatched clocks/sample rates, host block sizes, buffer underrun/overrun and no-reference fallback.
6. Verify startup/shutdown, plugin reload, state persistence, UI reopen and device reconfiguration.
7. Prove the mic is not present in reference and the far-end reference is never added to mic output.
8. Measure reported latency, actual latency and worst-case callback time separately.
