# Experimental Windows VST3

The optional plugin builds an actual native x64 VST3 module around the portable
AEC engine and asynchronous WASAPI reference capture. It is an experimental
integration; host/device and physical audio acceptance gates remain open.

Build with Visual Studio 2022, a Windows SDK, Git and CMake 3.25 or newer:

```sh
cmake -S . -B build-vst3 -A x64 -DBUNCANCEL_BUILD_VST3=ON
cmake --build build-vst3 --config Release --target BunCancel --parallel
```

The SDK validator runs after linking. The `Experimental Windows VST3` GitHub
Actions workflow additionally locates and validates the complete bundle and
uploads an unsigned experimental artifact. A workflow definition is not evidence
that a Windows build or validator run has passed. Install the complete
`BunCancel.vst3` directory into the host's VST3 search location; building does not
install it or modify any audio routes.

## Reference selection and lifecycle

Use the host's generic parameter editor. Select **Playback reference (no mic
monitoring)**, then turn on **Enable experimental AEC**. The default is dry mic.
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
host (or restart the host) after topology changes to refresh the generic picker.

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
remain with redistributions. No VST logo is included. The experimental package is
unsigned; no signing, public release or real-world compatibility claim follows
from compilation or validator success.

## Outstanding acceptance evidence

SDK validation covers host-interface conformance, not acoustic quality. Before
claiming Gate C/D, collect real Windows load/unload and state-reload results,
64–2048-sample blocks, bypass transitions, callback allocation/deadline evidence,
actual latency, hot device changes/disconnect, format mismatch, drift, reference
contamination, and physical Wave Link 3 plus ordinary-device recordings. No such
hardware results are supplied by this integration.
