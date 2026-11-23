# Checking playback and cancellation in Wave Link

Use **Record local diagnostic** only for a moment you are comfortable saving. The live meters work without recording anything to disk. Quiet playback, independent speech and an echo-free room are normal Ready conditions.

## Check the playback route

1. Keep BunCancel on the **mono microphone** input. For the reported setup, select **[Capture] Personal Mix (Elgato Virtual Audio)**, enable AEC and leave bypass off.
2. In Wave Link, check that Browser feeds Personal Mix and that the mix is monitored on the headphones you hear. Keep the microphone out of this playback reference. Wave Link mixes have independent routing, and its ear icon identifies the monitored mix. [Wave Link 3 setup and monitoring](https://help.elgato.com/hc/en-us/articles/46941178829585-Wave-Link-3-0-Software-Initial-Setup)
3. During ordinary playback, compare **raw reference**, **downmixed reference** and **mic** in BunCancel's three-second window. An endpoint name establishes how Windows exposed the device. The captured signal is the evidence that the route carries the intended program.

| Reading | What it tells you |
| --- | --- |
| Last block quiet, recent window active | A short pause or a quiet passage is visible. At 48 kHz a 512-sample mic block lasts 10.7 ms. |
| Recent packet receipt, raw and downmixed reference quiet throughout | The endpoint delivered low-level PCM. If the headphones carried clearly audible playback throughout that window, check which mix carries that playback. |
| Raw reference active, downmixed reference much quieter | Stereo channels may partially cancel in the mono downmix. The acoustic filter receives the downmixed reference. |
| Packet receipt age rising or low reference coverage | The plugin received few fresh samples. The last-underrun age separately reports when a usable block was last missing. |
| Both signals active, lag unconfirmed | The signal has yet to establish a repeatable echo path. Compare live and competing peaks across the window. |
| Aligned, output unchanged | Compare filter age, adaptation freeze fraction, shadow-filter validation and the actual changed-sample count. A delay estimate alone cannot show whether audio was subtracted. |

The activity indicator uses an RMS threshold of −60 dBFS for reporting. It does not gate Ready, delay acquisition or subtraction. The meter keeps aggregate energy, sample counts and peaks for about three seconds. It does not keep an always-on audio recording.

## Save three seconds locally

1. With host processing running, click **Record local diagnostic** and read the confirmation. The default choice is **No**. Confirming once authorizes the next three seconds of microphone, playback reference and returned output for this plugin instance. Use **Cancel** to discard an unfinished capture.
2. Wait for **Saved locally**. The editor shows the new folder under `%LOCALAPPDATA%\BunCancel\Diagnostics`. Each successful capture has its own folder. The plugin never uploads these files. The WAVs can contain intelligible voices. Delete the folder when you have finished with it.
3. To check the route by ear, listen to the saved WAVs locally after the capture. Do not route them back into the live microphone chain.

| File | Contents |
| --- | --- |
| `mic_input.wav` | Mono float32 mic samples supplied to the processor. |
| `reference.wav` | Mono float32 playback reference on the mic timeline, after stereo downmix, clock mapping and the 20 ms reference holdback. Unusable reference intervals are zero-filled and identified in the timeline. |
| `processed.wav` | Actual mono float32 output samples returned to the host during the same capture. |
| `timeline.json` | Relative timing, sample offsets, reference validity, interruptions and filter evidence. Absolute timestamps, endpoint IDs, account names and local paths are omitted. |

## Analyze without sharing audio

The separate Windows diagnostic tools download contains `analyze_capture.py` and `buncancel-offline.exe`. With Python 3.10 or newer, open PowerShell in the extracted tools directory and substitute the folder the editor displayed.

```powershell
py -3 .\analyze_capture.py "$env:LOCALAPPDATA\BunCancel\Diagnostics\capture-1" --runner .\buncancel-offline.exe
```

The default report summarizes the captured samples and the last shadow-filter validation outcome. Add `--blocks` to inspect individual callback records or omit `--runner` to inspect only the captured host output. The report measures recorded output against recorded mic samples. Replay starts a fresh filter on usable reference segments. The running plugin may already have learned a path before the capture began, so replay and host output can differ. A candidate lag in the saved reference is measured relative to its already-shifted samples. An unlabeled field recording has no isolated near voice or echo stem from which to compute ground-truth ERLE.

In the reported quiet snapshot the mic was −77.9 dBFS RMS, the most recent reference meter was −74.7 dBFS RMS, and all 512 mic samples passed unchanged. The peak of 0.498 at 263.10 ms was unconfirmed. The 9.65 seconds was elapsed time since the last underrun. This snapshot cannot identify the program that was audible in the headphones or establish what happened during the earlier, louder interval.


## What the automated host test covers

The Windows signal test calls the production VST3 processor through its mono audio interface with generated stereo reference packets and a controlled clock. It compares the host output with clean near-end and echo-only intervals, exercises in-place bypass, and checks that an explicitly requested three-second export matches the host samples. Its clock and reference backend exist only in the test executable. The packaged plugin runs a separate native editor/lifecycle smoke test and the SDK validator. Those checks cannot establish which program reached the physical headphones. The opt-in files let you check that locally.

CI uses generated audio in temporary test directories. Its audio artifact contains only the synthetic development fixtures. The plugin's local diagnostic folders are outside the upload paths.
