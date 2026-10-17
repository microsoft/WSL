# Experimental virtio-sound capture

The experimental Windows host backend uses Linux's existing `virtio_snd` driver.
Its intended result is an ALSA capture card in `/dev/snd` that an application or
container can open through the ordinary Linux ALSA interface. The implementation
is experimental and disabled at build time and at runtime by default.

## Configuration and kernel dependency

Build WSL using the [developer instructions](../dev-loop.md), with
`-DWSL_ENABLE_EXPERIMENTAL_VIRTIO_SOUND=ON` supplied to CMake. The normal build
leaves this option off. The experimental package includes `wsl-audio-worker.exe`.
The guest kernel must contain `CONFIG_SND_VIRTIO=m` and its ALSA dependencies,
with modules built for the exact running kernel. Enabling the host option does
not install or replace a guest kernel.

The following experimental settings select the deterministic synthetic source:

```ini
[experimental]
virtioSound=true
virtioSoundWindowsInput=false
```

Safe mode and disabling virtio disable the device, clear the physical-input
request, and clear its endpoint. When the device remains enabled,
configuration adds `virtio_snd` to the guest module list. Both the device and the
driver must initialize successfully before `/dev/snd` exists. Container device
mapping alone cannot create the host device.

Physical capture additionally requires `virtioSoundWindowsInput=true` and an
explicit `virtioSoundCaptureEndpoint` Windows capture endpoint identifier. It
does not silently choose a default microphone. Do not include real endpoint
identifiers or captured PCM in public diagnostic logs. The physical path remains
subject to separate runtime acceptance and should be exercised only in an opted-in development
environment. Guest PCM START causes microphone access; selecting the feature is
not a per-application permission prompt.

## Process and data path

`WslCoreVm` describes the external FlexibleIov device in the VM creation schema
and starts a VM-owned child of the service before starting the VM. The child
hosts the HDV callbacks, the virtio PCI device, and the control/event/TX/RX
virtqueues. The default source generates a deterministic 48 kHz, stereo S16_LE
ramp. This tests the actual guest driver and device path without an endpoint.

For physical input, the child starts an authenticated worker in the originating
user session using a restricted user token. The worker opens the explicit WASAPI
endpoint and sends validated PCM/control messages over a local pipe. The host
checks stream epoch/generation and copies complete frames into a bounded ring.
Guest DMA consumes only available hardware frames. Synthetic capture uses a
monotonic elapsed-time frame budget instead.

The core retains a PCM START control response while worker readiness is pending.
It acknowledges START only after a validated current-generation Started message;
startup failure returns an I/O error. Reset or a guest-memory fault invalidates
the retained chain. A 750 ms worker-start deadline is intended to fit within the
guest control timeout; scheduling and controller I/O are not hard-real-time.

The first valid retained WASAPI packet establishes the stream baseline. Later
discontinuities, timestamp errors, and ring overflow fail the stream. This policy
does not promise lossless data before that baseline, automatic reconnection,
arbitrary formats, playback, or long-running stability.

## Lifetime and security review

The service confirms guest shutdown before signalling graceful host retirement.
Parent death and unconfirmed guest exit use the failure/job-abort path. HDV
callback state remains rooted until child exit when ownership of the native host
registration cannot be proven; this avoids tearing down another registration.

The current HDV child retains SYSTEM identity at High integrity. A separate
process and kill-on-close job are lifetime boundaries, not a privilege sandbox
for guest-controlled descriptors. Minimum privileges, coexistence with WSL's
existing device host, and supported HCS/HDV ownership semantics need maintainer
and security review before enabling this feature in a shipping build. The WASAPI
worker's restricted user token does not remove the HDV child's review requirement.

## Offline tests and runtime validation

From the repository root, run the standalone suite with CMake, Visual Studio
C++ tools, and a Windows SDK installed:

```powershell
cmake -S src/windows/virtiosound -B out/virtio-sound-tests -G "Visual Studio 17 2022" -A x64 -DWSL_VIRTIO_SOUND_BUILD_TESTS=ON
cmake --build out/virtio-sound-tests --config Debug --parallel 1
ctest --test-dir out/virtio-sound-tests -C Debug --output-on-failure
```

The tests use fake guest memory, device callbacks,
and capture sources; they do not launch WSL, create a VM, or open an audio endpoint.
Windows token tests inspect only the current test process's token.

Standalone Windows builds can fetch the pinned WIL headers. To use existing
headers without downloading them, pass
`-DWSL_VIRTIO_SOUND_WIL_INCLUDE_DIR=<directory-containing-wil>`.
The offline GitHub workflow runs this suite, while the WSL PR pipeline builds
the opt-in backend for x64 and ARM64. TAEF configuration tests participate in
the repository's normal Windows test suite. CI compilation does not enable the
device or open a microphone in a guest.

Guest validation must separately check PCI/virtio enumeration, module binding,
ALSA card/PCM metadata, exact synthetic ramp samples, and repeated stream
start/stop. A container test should open the same guest card with only `/dev/snd`
mapped. Physical validation must additionally exercise the real worker startup,
selected endpoint, device loss, clock pacing, underrun, and cleanup.

The standalone [ALSA capture probe](../../../tools/test/virtio-sound-alsa-probe/README.md)
is an independent example for reading an explicitly selected guest ALSA device.
Its mock checks do not validate physical capture or device mapping under WSL;
those remain pending.

Record results against the exact source revision, WSL package version, matched
kernel/module pair, and input backend. Earlier binary results are not runtime
validation of changed source. WSL package version numbers and the WSL1/WSL2
architecture designation are separate version axes.
