# Experimental Windows VST3

The optional plugin builds an actual native x64 VST3 module around the portable
AEC engine and asynchronous WASAPI reference capture. It is an experimental
integration; host/device and physical audio acceptance gates remain open.

## Install or upgrade the Windows bundle

1. Use the editor-enabled build (plugin version 0.1.1). Extract the ZIP to a staging directory outside every VST3 search path. Check that it contains a folder named `BunCancel.vst3`, with `Contents/x86_64-win/BunCancel.vst3` and `Contents/Resources/moduleinfo.json` inside.
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

A physical Wave Link 3 test confirmed that the previous bundle loaded but opened a blank editor window. The replacement controller provides its own VST3 editor, attached to the host's Windows parent window, using native controls. Wave Link verification of the replacement editor and physical echo measurements are separate acceptance steps.

Select the endpoint carrying the playback you want to cancel, then enable AEC. Choosing **None** or switching on **Bypass** sends the dry microphone through. The editor displays the processor's status, including reference acquisition, device or format errors, alignment search and active cancellation. An alignment indication describes the filter state. It does not measure acoustic attenuation.

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

Device start, switch and shutdown run on a control worker. Audio processing
consumes fixed-size timestamped packets without allocating, locking or calling
Windows audio APIs. Captured generation changes, device-position discontinuities,
queue loss, packets more than 100 ms old, underrun, no selection, bypass and disconnected capture invalidate
alignment and select the dry path. The implementation retries capture only after
an explicit disable/enable or endpoint change; it does not repeatedly reconnect an
unavailable device. State loading is bounded to a 4096-byte endpoint identifier.

Processing declares zero additional output samples of latency: the microphone is
not delayed. Reference buffering affects the relative alignment search, not the
host's output latency. Timestamped capture is retained, but host/capture clock
mapping and drift correction are not implemented; equal nominal sample rates do
not establish synchronized clocks. Different sample rates fail dry. Delay search
can only recover within the DSP engine's documented range, so this integration
must not be presented as proven cancellation on arbitrary hosts or wireless paths.

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
contamination, and physical Wave Link 3 plus ordinary-device recordings. No such
hardware results are supplied by this integration.
