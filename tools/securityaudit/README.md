# Built-in static Linux audit payload

WSL owns the collector source in `src/linux/securityaudit` and the protocol-v2
event schemas migrated from WSL-SecurityAudit in `src/shared/securityaudit`.
Building does not require the prototype repository. This is not a plugin. The
collector is a separate process, not part of PID 1. Windows selects event
families through the existing configure/READY/STOPPED protocol.

## Build inputs

Build the independent, fully static executable on a native **x86_64** or
**aarch64** Linux builder. Build each architecture separately; do not relabel
an x64 package as ARM64. A glibc static executable is supported because it
shares no C++ runtime ABI with WSL's musl-based init.

Required inputs:

* The matching WSL source checkout used to generate the payload fingerprints.
* An authorized WD.Client.Linux.eBPF checkout with the `build.sh -S` static
  build support, its pinned libbpf submodule initialized, and its existing
  Clang/LLVM 20 BPF prerequisites. The static option preserves the default
  shared-library build and changes neither events nor collection semantics.
  The companion implementation is commit
  `9c6e2977af4f0435efa8150e755edc1e83c87c22` on `user/shawn/mdeSupport`
  in that repository; the branch in WSL does not include the external checkout.
* WSL's pinned nlohmann JSON **3.12.0** headers (for example the existing
  `_deps/<platform>/nlohmannjson-src/include` after WSL configuration).
* CMake 3.25+, Make, Clang/Clang++, readelf, Git, pkg-config, and the native
  static C/C++ runtime, libelf and zlib development archives. All compression
  archives listed by `pkg-config --static --libs libelf` are required too.
  A missing archive is a build failure, never a shared-library fallback.

The script does not install dependencies, fetch arbitrary source, run WSL,
change services, deploy, or disable other security software. Initialize the
authorized submodule/dependencies through the normal build provisioning
process when a missing dependency is reported.

Debian 13's `libelf-dev` 0.192-4 static archive has unresolved
`eu_search_tree_init`/`eu_search_tree_fini` references despite its pkg-config
metadata. If the **real collector link** reports this, build the actual
libelf and libeu implementations from the pinned upstream source, rather
than supplying replacement functions:

```sh
# Source: https://deb.debian.org/debian/pool/main/e/elfutils/elfutils_0.192.orig.tar.bz2
# SHA256: 616099beae24aba11f9b63d86ca6cc8d566d968b802391334c91df54eab416b4
bash tools/securityaudit/build-libelf-static.sh \
    "$ELFUTILS_ARCHIVE" "$PWD/build-audit-libelf"
export PKG_CONFIG_PATH="$PWD/build-audit-libelf/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
```

This helper verifies the archive and emits a local static-only dependency
package with correct libeu linkage metadata. It neither replaces system
libraries nor changes collection behavior. If reusing a failed payload build
directory, clear its CMake cache using `cmake --fresh` with the same inputs
so library discovery selects the corrected local package.

Run on the Linux builder, substituting paths to the actual checkouts:

```sh
bash tools/securityaudit/build-payload.sh \
    "$EBPF_SOURCE" "$JSON_INCLUDE" \
    "$PWD/build-audit" "$PWD/audit-payload"
```

This uses the external library's supported `build.sh -S -j 1` entry point,
links its real archive directly with libbpf/libelf/zlib, and emits the full
collector. There are no `dlopen`/`dlsym` loader calls, `ld-linux`, `.so` files,
runtime-library trees, or external plugin directory mounts in the payload.

On the Windows build:

```powershell
cmake . -DWSL_SECURITYAUDIT_PAYLOAD_DIR="<matching architecture payload directory>"
cmake --build . -- -m
```

The `wsl_audit_collector` leaf target validates the source fingerprints (with
CRLF/LF normalized so Windows and Linux checkouts agree),
architecture, complete inventory, SHA-256 hashes, ELF program headers and
dynamic tags on **every build**, then stages to
`bin/<platform>/<configuration>/securityaudit`. A missing/stale/dynamic
package fails the build visibly; no placeholder collector is generated.
File hashing uses .NET streams directly, so the MSBuild-launched Windows
PowerShell 5.1 validator also works when invoked from PowerShell 7 without
depending on inherited module paths or a shell profile.
`initramfs` embeds the verified payload alongside the unchanged `/init`
entry. Both ordinary WSL2 and WSLC use this WSL-owned initrd; WSL1 is unchanged.
The existing MSI initrd packaging needs no separate runtime library payload.

## Exact guest payload

* `/wsl-audit-collector` — fully static native executable.
* `/wsl-security-audit/ebpf_wrapper_kern_raw_tp_btf_rbuff_enriched_fmap.o`
* `/wsl-security-audit/ebpf_wrapper_kern_raw_tp_sub4096_btf_rbuff_enriched_fmap.o`
* `/wsl-security-audit/ebpf_wrapper_kern_raw_tp_args64_krsi_btf_rbuff_enriched_fmap.o`
* `/wsl-security-audit/syscalls.conf` on x64, or `syscalls_aarch64.conf` on ARM64.
* `/wsl-security-audit/provenance.json` — package format, architecture,
  protocol/event ABI, exact collector source hashes, loader/libbpf revisions
  and dirty-diff hashes, archive hashes, compiler version and file hashes.

The three BPF objects cover the loader's existing kernel-selection paths for
the required BTF/ring-buffer/enrichment/feature-map configuration. Extended,
tamper-protection and unrelated fallback objects are not selected by the
current collection plan. BPF remains data loaded by the statically linked
library, not a dynamic userspace dependency. Archives are build inputs and
are not copied into the boot image.

The helper remains usable without loading BPF:

```sh
/wsl-audit-collector --inspect-pid 123 [--container-id container-id]
```

Namespace/cgroup validation and bounded data framing remain intact. Linux
identity claims remain untrusted input to Windows attribution. A
nonblocking `/run/wsl-security-audit.lock` prevents overlapping native
collector instances in the VM; helper inspection does not acquire it.
The Windows owner performs bounded retry and must deduplicate the old
prototype plugin. No infinite retry or synchronous boot wait is added here.

A separate lightweight init child listens on vsock port **50007** in both
WSL2/WSLC roots before the main host control channel is opened. It accepts
host-CID connections only, bounds launch frames to 64 KiB/16 arguments and
five-second I/O, and allowlists `/wsl-audit-collector`. The existing
CREATE_PROCESS handshake produces an independent duplex data socket.
Closing the launch control channel does not close collector stdio. Listener
failure is logged and does not abort boot; no BPF work or accept loop runs in
PID 1. WSL1 does not enter these boot paths.

The listener retains the initrd root while ordinary WSL2/WSLC init can move its
proc/sys mounts into a new root. Before checking kernel BTF or loading BPF, the
collector creates a private mount namespace, disables mount propagation, and
mounts real procfs/sysfs if missing, followed by tracefs if needed. These mounts
exist only in the collector and disappear on exit; PID 1 and distribution mounts
are unchanged. Namespace inspection similarly prepares private procfs when
missing, without loading BPF or mounting tracefs. Missing BTF or mount/permission
failures still fail collection; no synthetic BTF or degraded-success fallback is
used. Repairing missing kernel mounts requires CAP_SYS_ADMIN.

## Validation

First complete the coordinated **full WSL build** before running tests.
On the Linux builder:

```sh
ctest --test-dir "$PWD/build-audit" --output-on-failure
```

`AuditCollector`, `AuditProtocol` and `AuditObservation` exercise the imported
decoders, namespace/cgroup validation and existing schema contracts against
the real linked loader archive. `AuditStaticElf` inspects the **full collector**
and rejects `PT_INTERP` or `DT_NEEDED`; it is not a tiny static-link probe.

For privileged integration on an authorized, recovered test VM with no
other native collector, enable the explicit live test and use the same build
directory and existing configuration:

```sh
cmake -S tools/securityaudit -B "$PWD/build-audit" -DWSL_AUDIT_ENABLE_LIVE_TESTS=ON
cmake --build "$PWD/build-audit" --target audit_payload --parallel 1
sudo ctest --test-dir "$PWD/build-audit" -R '^Audit(LiveCollector|MinimalRootNetwork)$' --output-on-failure
```

This test runs the actual static collector, requires real BPF attachment,
executes `/bin/true`, requires its actual process event and a successful
STOPPED acknowledgment. Failure/timeout is **not** converted into success or
a capability skip. It does not stop services or forcibly kill processes.
Capabilities must allow BPF/perf (or CAP_SYS_ADMIN on older kernels);
tracefs namespace setup may also need CAP_SYS_ADMIN. Kernel BTF and the
existing required BPF features must be available. Inspect stderr and protocol
errors for attachment/verifier/permission diagnostics.

`AuditMinimalRootNetwork` runs the same real collector in a fresh private
mount namespace and chroot containing only the trusted payload plus **empty,
unmounted** proc/sys/run directories. It has no libc or dynamic interpreter tree.
The test verifies real procfs/sysfs/tracefs and readable kernel BTF in the
collector's distinct mount namespace, and that the parent root's proc/sys stay
empty both during collection and after the collector exits. It also executes
the actual `--inspect-pid` helper from that root before collection, verifying the
target's real namespace identities without pre-mounting procfs. It
requires a real exec and socket event, **zero attached DNS probes**, explicit
`dnsCoverage: unavailable`, and no `dns-query` in runtime supported coverage.
The library's new read-only probe-count diagnostic does not change event
formats, attachment decisions or partial-attachment behavior.

PowerShell payload/initrd regression coverage is in `Test-Payload.ps1`.
Its synthetic ELF fixtures test the validator only, not real BPF loading.

```powershell
powershell -NoProfile -File tools\securityaudit\Test-Payload.ps1
```

The isolated launch-signal regression uses the production child signal-policy
helper with Linux abstract Unix sockets. A cancelled reply must return EPIPE,
the same listener must accept a second request, and the parent must retain its
original SIGPIPE disposition. It does not start an init/service or test the
installed Windows-to-vsock handshake:

```sh
cmake -S tools/securityaudit/tests -B build-audit-launch-tests -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build-audit-launch-tests --parallel 1
ctest --test-dir build-audit-launch-tests -R '^AuditLaunchSignal$' --repeat until-fail:10 --output-on-failure
ctest --test-dir build-audit-launch-tests -R '^AuditLaunchRequest$' --output-on-failure
```

`AuditLaunchRequest` compiles the actual private launch request handler and shared
socket/transaction decoder, substituting only process creation and init logging.
It requires Clang with C++20 `<format>` support and the repository's restored GSL
headers (or `-DWSL_AUDIT_GSL_INCLUDE=<include-directory>`). The stream transport
rejects `MSG_PEEK`, matching Hyper-V sockets. Coverage includes fragmented headers
and bodies, the 64 KiB limit before body allocation, the unchanged 16 MiB global
ceiling, executable/argument validation, truncated frames/EOF, independent
transaction IDs and reply steps, and stalled reads at the production five-second
socket timeout. Linux receive transactions use no timeout parameter; socket
timeouts bound individual blocked I/O calls, not an overall transaction deadline.
The test does not launch a collector, change audit policy, or prove the installed
Windows-to-vsock handshake. These tests and launch changes do not alter the
static collector's `SourceIdentity.cmake` inputs or require restaging that payload.

## Deliberately unchanged limitations

DNS probes still target libc visible in the collector's captured root.
Putting a static collector in initrd does **not** establish distribution or
container DNS coverage. Missing root libc leaves the existing nonfatal DNS
probes unattached; kernel network syscall collection stays enabled. READY
reports the actual attached count and removes unavailable DNS from runtime
coverage; stderr reports the limitation. A positive count is still only
captured-root coverage, not verified distribution/container attribution.
Older credential/capset/mount/time
field limitations and existing family coverage remain unchanged. No new
event family, event layout, attachment behavior, or MDE bypass is introduced.

An inaccessible Linux build/test environment means the static artifact and
privileged validation are **not complete**, even if Windows-side syntax and
packaging checks pass. Do not ship until both architectures and the real
loader test have been validated.

Before redistribution, service the licenses/source obligations of every
statically linked dependency (including libbpf, elfutils/libeu, compression
libraries and the C/C++ runtime) through WSL's release-compliance process.
The provenance/archive hashes and pinned-source build helper are not a
substitute for those obligations.
