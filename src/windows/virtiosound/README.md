# Experimental native virtio-sound backend

This backend exposes an ALSA capture device through Linux's existing
`virtio_snd` driver. The portable C++17 device core models modern virtio PCI,
bounded split-virtqueue traversal, guest-memory scatter/gather and interrupts.
The Windows adapter provides HDV callbacks and an optional user-session WASAPI
worker. A deterministic stereo 48 kHz S16_LE ramp is the default PCM source.

The feature is disabled by default. The [architecture and configuration
notes](../../../doc/docs/technical-documentation/virtio-sound.md) explain
its VM integration, kernel dependency, and security/runtime review gaps.

## Offline tests

From the repository root, with CMake and a C++ toolchain installed:

```powershell
cmake -S src/windows/virtiosound -B out/virtio-sound-tests -G "Visual Studio 17 2022" -A x64 -DWSL_VIRTIO_SOUND_BUILD_TESTS=ON
cmake --build out/virtio-sound-tests --config Debug --parallel 1
ctest --test-dir out/virtio-sound-tests -C Debug --output-on-failure
```

The standalone project avoids configuring or installing the full WSL package.
It uses the Visual Studio generator rather than a machine-specific installation
path. Tests run as an ordinary user and use fake guest memory, HDV functions,
capture devices, and worker hosts. They do not create VMs or open a microphone.
Windows token/security tests inspect the current token and in-memory security
descriptors. The native adapter is also compiled against the installed Windows
SDK. CTest registration does not execute tests during the build.

The full WSL build uses the repository's WIL NuGet dependency. A standalone
Windows build downloads a hash-pinned WIL source archive if WIL is not already
available. For an offline build, pass
`-DWSL_VIRTIO_SOUND_WIL_INCLUDE_DIR=<directory-containing-wil>`.

The standalone suite runs in the `Virtio sound offline tests` GitHub workflow.
The WSL PR pipeline also compiles the feature on x64 and ARM64; production build
defaults and runtime defaults remain off. Repository TAEF tests in
`test/windows/common/VirtioSoundConfigUnitTests.cpp` cover configuration defaults,
endpoint validation, and safe-mode/transport overrides. Run them through the
repository's normal full-build and `test.bat` workflow.

Portable device/ring tests can be built on Linux using the default CMake
generator; Windows-only worker, token, pipe-security and adapter tests are
excluded there. Full WSL service/MSI builds require the repository's normal
developer prerequisites and `-DWSL_ENABLE_EXPERIMENTAL_VIRTIO_SOUND=ON`.

## Stream lifecycle

Synthetic capture accumulates bounded frame credit from a monotonic elapsed
timer and discards stale credit on lifecycle changes. Hardware capture reports
the actual number of available ring frames and cannot manufacture samples from
elapsed time. Pending START completion is retained until the current worker
generation reports readiness. Failure reports I/O error; reset and guest-memory
faults invalidate pending responses. The first retained WASAPI packet establishes
the input baseline; later discontinuities or timestamp faults fail the stream.

The [PCM ring](S16LePcmRing.md) uses generation-tagged producer writes and whole
frame accounting. Overflow and genuine underflow remain observable failures.

## Validation boundary

Offline tests establish protocol and lifecycle behavior under their fake inputs.
They do not prove HDV support on a Windows build, privilege isolation, actual
WASAPI scheduling, sample timing, reconnect behavior, or packaging signatures.
The host VM and physical capture paths require separate runtime acceptance.
Passing the offline suite is not evidence that a particular WSL package,
kernel/module pair, or microphone works. Keep test results tied to the exact
source revision, platform, and input backend that was exercised.

The new HDV child still retains SYSTEM identity. Its process/job boundary does
not make it a privilege sandbox; maintainer security review is a prerequisite
for shipping. Keep the backend opt-in until the native VM and physical capture
acceptance checks are complete.
