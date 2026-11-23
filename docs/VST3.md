# Experimental Windows VST3

The optional plugin builds an actual native x64 VST3 module around the portable
AEC engine and asynchronous WASAPI reference capture. It is an experimental
integration; host/device and physical audio acceptance gates remain open.

## Install or upgrade the Windows bundle

1. Use the editor-enabled build (plugin version 0.1.4). Extract the ZIP to a staging directory outside every VST3 search path. Check that it contains a folder named `BunCancel.vst3`, with `Contents/x86_64-win/BunCancel.vst3` and `Contents/Resources/moduleinfo.json` inside.
2. Close Wave Link and any other hosts using the plugin. Save the current host settings. In the VST3 search directory, the existing `BunCancel.vst3` may be a single file or a folder with that same name. Move the **entire existing item** to a backup directory outside the scan paths. Keep the backup until the upgrade works.
3. Copy the **outer folder** from staging into the search directory. A typical resulting path is `C:\Program Files\Common Files\VST3\BunCancel.vst3\Contents\x86_64-win\BunCancel.vst3`. Leave the resources and inner binary together. Installing a second loose copy of the inner binary can make the host resolve the wrong plugin.
4. Reopen the host, rescan if available, and open BunCancel's editor on the microphone chain. The editor should show the playback-reference picker, enable and bypass controls, and processing status. To roll back, close the host, move the new bundle out of the scan path and restore the backed-up item under its original name.

## Build and test

Use Visual Studio with the Windows x64 C++ toolchain, a Windows SDK, Git and CMake 3.25 or newer.

```sh
cmake -S . -B build-vst3 -A x64 -DBUNCANCEL_BUILD_VST3=ON
cmake --build build-vst3 --config Release --target buncancel-editor-smoke validator --parallel
ctest --test-dir build-vst3 -C Release -R "^vst3-editor$" --no-tests=error --output-on-failure
```

The Windows workflow also runs the SDK's `validator.exe` against the full bundle and uploads the unsigned ZIP after the checks pass.

## Reference selection and lifecycle

A Wave Link 3 field test of 0.1.1 confirmed that the custom editor opened and listed Personal Mix as a capture endpoint. The host showed active microphone and playback meters while BunCancel stayed at “Dry - acquiring alignment”. That editor status established that reference samples reached the DSP. The host meters could not show the PCM levels inside BunCancel, their relative timing or how much playback reached the mic.

A subsequent 0.1.2 field observation showed a mono microphone at −34.0 dBFS RMS and the Personal Mix capture endpoint at −38.1 dBFS RMS. Both clocks reported 48 kHz. The displayed lag was 272 ms, the search peak was 0.747, and the packet offset was +3.79 ms. Those last two time readings describe different events. With 20 ms of reference holdback, the filter was using an effective lag of about 252 ms. The packet offset describes the latest capture delivery relative to the mic block. Neither value measures how much the filter subtracted. The 15 underruns were cumulative; their timing relative to the screenshot is unknown.

### Local Wave Link check

1. Put BunCancel on the microphone chain and select the endpoint carrying the playback you hear. Keep microphone monitoring out of that mix.
2. Enable AEC and leave bypass off. With a valid reference stream, **Ready** is the expected result even in a quiet room. The mic continues through and the engine monitors automatically. There is no calibration step.
3. Read the **BunCancel** mic and reference RMS/peak values during normal speech and playback. The two host mixer meters can be active even if the selected endpoint supplies different PCM. A silent plugin reference with audible playback points to the reference route. A silent plugin microphone during speech points to the plugin input route.
4. If reference interruptions accumulate, check device availability and the host and endpoint sample rates shown in the editor. A steady **Ready** with both meters moving can simply mean the playback produces no detectable mic leakage. **Cancelling** indicates recent aligned subtraction with measured reduction. The estimate is a live diagnostic, not a ground-truth ERLE score.

For a quiet block or persistent leakage, the editor also reports the last three seconds of mic, raw reference, downmix and output levels. Packet receipt age, last above-threshold activity and last underrun have separate labels. The optional three-second recording and offline analysis are described in the [local field guide](FIELD_DIAGNOSTICS.md).

The endpoint picker contains explicit render-loopback and capture endpoints
exposed by Windows, ordered by stable endpoint ID. An endpoint name alone does
not prove that it is a suitable far-end reference. Ensure microphone monitoring
is absent from that reference before enabling processing. Automatic detection of
reference microphone contamination is not yet accepted.

The plugin accepts mono float32 microphone input/output. Native stereo reference
is explicitly averaged as `0.5 * left + 0.5 * right`; anti-phase stereo can cancel
in this downmix and needs a later stereo-aware estimator. Endpoint ID, enable and
bypass survive component-state reload; a missing saved endpoint resolves to
**None**, never an automatically substituted device. Enumeration creates one immutable module-wide snapshot shared by every
controller and processor. This prevents topology changes between initialization
from mapping an index to the wrong reference. Fully unload the module from the
host (or restart the host) after topology changes to refresh the endpoint picker. Closing and reopening just the editor retains the same device list and selection.

Device start, switch and shutdown run off the audio thread. The callback reads timestamped capture packets from a bounded queue. The reference meter measures incoming PCM before stereo downmix and timeline validation. If WASAPI flags an unreliable timestamp, its PCM can still move the meter while the mic stays dry. The gap counter records the timing error. A discontinuity invalidates the time mapping and adaptive state, so the mic passes dry while the reference timeline refills. A rate mismatch leaves the microphone dry.

The callback estimates the microphone block start from its end time, block length and sample position when available. It looks up reference samples on a two-second timeline using WASAPI QPC timestamps and device positions. Interpolation tracks small clock-rate differences. A fixed 20 ms reference holdback absorbs packet scheduling jitter. The displayed packet offset is the mic block start minus the start of the latest capture packet. Candidate lag includes the holdback and can differ from packet offset. A weak peak is labelled unconfirmed.

The DSP can subtract at effective lags of two samples through 333 ms after holdback. It also checks excluded lags down to −40 ms and up to 500 ms for diagnostics only. A reference arriving too late for the current mic block, a very short path inside the holdback or a long wireless delay can still leave correlated leakage dry. The microphone has zero algorithmic lookahead. Reference timing uses WASAPI packet timestamps and an estimate of the host block time. Host buffering and device latency can contribute to the observed lag. A timestamp on a callback is not a measurement of when sound reached the microphone.

## When the filter aligns but leakage remains

The microphone path accepts mono. The stereo capture meter measures incoming samples before downmix. The delay search compares the downmixed reference with the mono microphone. A high search peak can come from a different lag or an earlier passage. Use the live confidence at the active lag to see whether that relationship persists. Repeating material can produce competing peaks at multiple delays.

The original foreground filter adapts conservatively to protect near-end speech. A change in the acoustic path can drive its prediction coherence below the update threshold, even during far-end-only playback. Once that happens, the same stale prediction can keep the filter frozen. Version 0.1.3 also learns a bounded candidate filter from correlated reference samples. It freezes the candidate coefficients and evaluates the prediction on later samples against both the dry microphone and the current output. Only a candidate that improves both can replace the audible filter. Both filters use the same finite tail and zero mic lookahead.

Use the editor's changed-sample count, output RMS and mic-to-output level change to see what reached the host on the most recent block. Adapted samples, prediction level, subtraction gain and the recent freeze fraction explain how that result arose. A low filter age together with recent underruns suggests interrupted learning. A stable filter age and a high freeze fraction suggest an acoustic model that is no longer tracking the mic, or simultaneous near-end speech. The live level change includes speech and cannot establish far-only ERLE.

### Post-alignment regression measurements

Run `ctest --test-dir build -C Release -V -R "^post-alignment$"`. Set `BUNCANCEL_FIELD_EXPORT` to a directory to save synthetic stems, processed WAVs and metrics. Tests use a 272 ms path, −38.1 dBFS downmixed reference, approximately −33.5 dBFS mono mic during the initial far-only window, and unequal left and right playback channels. After a path change at six seconds, far-only ERLE is measured from nine to ten seconds. The old and new engines receive identical samples.

| Synthetic path | 0.1.2 far-only ERLE | 0.1.3 far-only ERLE |
| --- | ---: | ---: |
| Stable reflection, gain change | 11.38 dB | 13.03 dB |
| Changed reflection | 3.70 dB | 13.14 dB |
| Changed reflection and polarity | 0.00 dB | 13.09 dB |

On the polarity stress case, the old engine stayed aligned with live correlation 0.748 and passed every mic sample through unchanged in the scored far-only second. The constrained subtraction gain had fallen to zero. In the changed-reflection case the new engine's projection onto clean near speech during double-talk was 0.997, and residual error energy was 1.56% of clean near-end energy. The near-only tail passed through exactly. The same changed reflection with a short capture interruption reached 12.33 dB far-only ERLE after recovery. These generated cases establish failure mechanisms and regression coverage. The cause of the field observation and physical attenuation remain open.

The established nine-fixture development matrix still includes difficult short, cold-start windows. The baseline far-only score increased from 2.10 to 2.47 dB and the drifting case remained at 0.75 dB. The 128-tap acoustic model spans only 2.67 ms at 48 kHz after bulk delay alignment. Longer room reflections can remain audible.

## Timing regressions measured in 0.1.2

Run `ctest --test-dir build-vst3 -C Release -V -R "^reference-(timeline|pipeline)$"` for the deterministic clock and signal scenarios. The pipeline test feeds identical generated reference and mic samples to a reproduction of the former FIFO policy and to the production timeline, host clock and AEC. At 48 kHz it scores the final two seconds of each six-second signal. The paths in this table have a strong direct component and one short reflection.

| Synthetic far-only case | Former FIFO ERLE | Timestamped ERLE |
| --- | ---: | ---: |
| 80 ms startup backlog, 40 ms echo path | 0.00 dB | 41.05 dB |
| −1000 ppm capture clock, uneven packet delivery | 0.00 dB | 29.52 dB |
| Ordinary 480-frame capture and 256-frame mic blocks | 41.17 dB | 41.04 dB |

With speech-like playback, settled far-only ERLE was 40.70 dB. During simultaneous independent speech, the projection of the output onto the clean near-end stem was 0.997 and residual error energy was 2.24% of near-end energy. Independent speech without bleed and speech over a silent reference were bit-exact dry. A silent mic produced exact zero, including after training. With ±1 ms callback scheduling jitter the far-only score fell to 13.12 dB. After a simulated 250 ms reference outage, alignment returned in 889 ms and the processor reported measured cancellation after 931 ms. The separate cold-start room simulations include acquisition and longer reflections. Far-only ERLE on those development fixtures was about 0.75–2.10 dB. On one Linux Release run, the additional delay search and reduction diagnostics increased offline DSP time by roughly 1.7–2.1× over 0.1.1. The largest measured 256-frame block took 0.421 ms against a 5.33 ms deadline. Those timings exclude device capture and the Wave Link callback. Physical attenuation, voice retention and deadline performance require a field measurement.

## Pinned dependency and licenses

Only the integration layer uses the VST3 SDK. The AEC engine is project-authored.
The SDK is pinned to
[`v3.8.1_build_84`](https://github.com/steinbergmedia/vst3sdk/tree/3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96),
commit `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`.
The pinned checkout's root and four fetched submodules were inspected for license
terms. Each supplies an MIT `LICENSE.txt`; original notices are copied into
`Contents/Resources/licenses` in the distributable bundle. VSTGUI, tutorials and
documentation submodules are not fetched or used.

| Component | Pinned commit | License |
| --- | --- | --- |
| SDK root | `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96` | MIT |
| base | `fcf9da0bd27a16f7f03773a3a39822f28f5c8477` | MIT |
| cmake | `054c9143cbb8d47fc4694e473f2ee3b4d951a8f5` | MIT |
| pluginterfaces | `4f547e8e102b47de4a8b8aaf343c73b700786372` | MIT |
| public.sdk | `586dc5e6c8012c3e4b01c79389375cbe96bdb1da` | MIT |

The [official licensing guidance](https://steinbergmedia.github.io/vst3_dev_portal/pages/VST%2B3%2BLicensing/VST3%2BLicense)
explains MIT licensing and separate trademark usage rules. The SDK notices must
remain with redistributions. The SDK supplies the Windows bundle icon. The experimental package is
unsigned; no signing, public release or real-world compatibility claim follows
from compilation or validator success.

## Outstanding acceptance evidence

SDK validation covers host-interface conformance, not acoustic quality. Before
claiming Gate C/D, collect real Windows load/unload and state-reload results,
64–2048-sample blocks, bypass transitions, callback allocation/deadline evidence,
actual latency, hot device changes/disconnect, format mismatch, drift, reference
contamination, and physical Wave Link 3 plus ordinary-device recordings. The reported editor and endpoint observations do not include physical attenuation or voice-retention measurements.
