# Native Linux security auditing

WSL owns one best-effort collector for each ordinary WSL2 VM and each WSLC VM
generation. This is a core service feature, **enabled by default**, not a plugin.
It does not load an audit DLL or use a host-configured payload directory or plugin
mount. Both boot images contain `/wsl-audit-collector` and
`/wsl-security-audit`; the build must validate a genuinely static collector and
its complete, architecture-matched BPF package.

## Policy and availability

Default-on does **not** enable Windows audit subcategories. Collection continues
to follow the existing prototype mappings:

| Linux observations | Windows audit subcategory |
| --- | --- |
| Process execution and supported process creation | Process Creation success |
| Network successes/failures | Filtering Platform Connection success/failure |
| Module observations | Security System Extension success |
| Privilege transitions | Authorization Policy Change success (`0cce9231-69ae-11d9-bed3-505054503030`) |

Command lines additionally require the machine Windows policy
`ProcessCreationIncludeCmdLine_Enabled=1`. The Security sink also requires
Application Generated success auditing. WSL does not change these policies or
grant `SeAuditPrivilege`; the writer must already hold that privilege.

Administrators may set the 64-bit machine policy
`HKLM\SOFTWARE\Policies\Microsoft\Windows\WSL\SecurityAuditEnabled` (`REG_DWORD`)
to `0` to disable native collection for subsequent VM starts. Missing means on;
`1` means on. Malformed values are diagnosed and cannot disable collection.
There is deliberately no HKCU, environment-variable, or `.wslconfig` switch.

The coordinator schedules startup off the lifecycle callback, waits at most ten
seconds for protocol-v2 READY, and makes at most three collection attempts per
VM generation (one- then two-second retry backoff). Under lifecycle locks, launch
captures only the VM's transport GUID and a duplicate of its termination event.
The owning VM, its main init channel, and its idle lifetime are not retained.
All process-create IPC happens after those locks are released, over a dedicated
host-only guest listener on port 50007 using the existing core create-process
handshake. Each request has its own channel, isolated from workload startup.
The connector uses nonblocking polling and a two-second deadline; normal core
launch paths retain their existing behavior. WSLC checks the generation before
capture and also requests COM cancellation after two seconds. OS cancellation
is not a hard real-time termination guarantee, but it cannot hold a VM lifecycle
lock or stop a replacement VM from starting. Neither retry backoff nor event
reception runs under lifecycle locks.

Stop signals cancellation without joining a worker or issuing cleanup RPCs.
The worker independently attempts bounded STOP/STOPPED draining, then closes the
transport; VM teardown is never delayed to await it. Missing final counters are
diagnosed. The guest collector's nonblocking VM-wide lock prevents retries
from attaching a second collector while the previous process remains alive.
Failures and exhausted retries are visible through `NativeAuditStatus` and
`NativeAuditDiagnostic` on WSL's existing diagnostic trace provider, not returned as workload
startup failures. Validated READY/STOPPED counters are diagnostics; Linux security
observations are sent **only** to the real Authz Security sink, with no ETW,
Application log, or file fallback.

## Identity and event compatibility

The Windows service supplies the session user's SID and Windows session ID.
Ordinary WSL uses distribution GUID/name/PID-namespace notifications. WSLC uses
its own session ID/name and typed container ID/name/init PID/start time, not
plugin inspect JSON. A guest helper validates the live PID, namespaces, cgroup
and observation time before container attribution. Failure, ambiguous namespace
matches, stale instances, and generation changes leave identity unknown rather
than guessing. A stopped VM's worker cannot launch into its replacement.

The existing protocol-v2 validators, supported event fields, projections and
experimental event identifiers are retained. Namespace/time observations and
the existing capset gaps are **not** newly enabled. DNS requires a matching libc
probe target in the collector root; the packaged minimal root currently has no
such target, so its DNS coverage is unavailable. Distribution/container DNS
coverage is not established. READY explicitly reports
`dnsProbeCount` and `dnsCoverage` when network auditing is requested. Zero probes
reports `unavailable` and removes only `dns-query` from the advertised supported
coverage; socket observations remain enabled. Initialization diagnostics are a
boolean, not untrusted guest text. This migration does not
claim to correct those coverage limitations, prevent deliberate privileged guest
tampering, or guarantee Security-log durability.

## Source ownership and installation

The native source is **Microsoft-Windows-WSL-SecurityAudit**. It deliberately does
not reuse `WSL-SecurityAudit`, the prototype's historical source. Its message
resource is the resource-only `wslsecurityaudit.dll`, not a collector plugin.
The experimental IDs (including 62010/62011 and the existing observation IDs)
are not assertions of production allocation or client Authz contract approval.
Production source/event-ID and deployment approvals remain outstanding.

MSI commit actions register/read back the owned source on successful
install/upgrade/repair, and deregister it on successful final uninstall.
The native message resource and historical message lookup metadata are retained.
Upgrade removal does not unregister the new installation's source; rolled-back
transactions do not perform source mutation. Source conflicts are explicitly
logged and are not silently overwritten. Registration errors are availability-
first and leave a diagnostic, not a file-output fallback.

The loader skips only the exact legacy registration name
`WSL-SecurityAudit-Prototype`, preventing double collection. It does not delete
that registration, its source, or its DLL. Keep the legacy DLL at the historical
message-file path if old Security events need rendering. Other plugins,
including MDE, retain their existing behavior.

## Validation

### Controlled installed x64 validation (2026-10-22)

A full Debug build was installed on an explicitly approved Windows host.
Verification used the native source and actual Linux syscalls, not replay or
rewritten prototype events:

- Ordinary WSL2 produced 67 verified records covering all 12 current IDs:
  62010, 62011, 62020-62024, 62030-62031 and 62040-62042.
- Two independent WSLC sessions each produced 43 container-attributed records
  across the ten process/network/identity IDs, plus both successful module-load
  IDs from their VM roots. VM-root module events correctly had no container
  identity. Each session's VM idled down and restarted with a new boot ID,
  retained its session identity, and produced the ten container event types
  again without cross-session attribution.
- All selected events passed strict native-provider, six-insertion XML and
  General-description checks. All six controlled module insertions were
  followed by successful unloading of only the fixture loaded by that probe.
- A blocking loopback connect to a bound, non-listening socket produced a
  matching `ECONNREFUSED` event. Disabling network auditing suppressed network
  records while process execution/creation continued. Asynchronous
  `EINPROGRESS` remains pending, not a fabricated final connection outcome.
- MDE remained loaded, the legacy audit plugin was not loaded, owned test
  sessions were closed, and all 60 Windows audit subcategories returned to
  their pre-test values. Default-on native auditing does not imply network
  auditing remains enabled after this restoration.

Installed testing exposed two requirements that component tests alone had not
proved: Hyper-V vsock does not support `MSG_PEEK`, and the early listener's root
can lose proc/sys mounts when PID 1 moves them. Launch framing now reads without
peeking and checks its 64 KiB limit before body allocation; the collector prepares
real proc/sys/tracefs mounts in its private mount namespace before BTF checks.
The minimal-root regression deliberately does not pre-mount proc/sys.

This result does not establish DNS, capset, mount/time events, ARM64 runtime
coverage, production event-ID approval, or long-duration performance/reliability.
Native source uninstall and historical rendering after uninstall still require
a separate servicing test. Security log retention must be checked during
acceptance; preserve exported EVTX and original XML/General snapshots before a
circular log overwrites earlier records.

`NativeAuditUnitTests` covers policy default-on/negative cases, protocol startup,
exit and malformed input, host identity isolation, unknown attribution, finite
startup retries, nonjoining stop, independent generations, slow in-flight launch
during VM destruction/replacement, DNS coverage diagnostics, source separation,
and the built MSI's native WSLC proxy registration (13 test methods).
As with all native WSL tests, run the **entire** project build before tests:

```powershell
cmake --build . -- -m
& .\packages\Microsoft.Taef.10.100.251104001\build\Binaries\x64\te.exe `
    .\bin\x64\Debug\wslaudittests.dll /name:NativeAuditUnitTests::*
```

`wslaudittests.dll` intentionally excludes the shared `Common.cpp` module setup:
this direct unit-only command does not install WSL, import a distribution, change
the default distribution, or rewrite `.wslconfig`. Do **not** substitute
`test.bat`: its default setup can remove/install packages and prepare a distro.
Even its fast mode still enters the shared module's configuration/default-distro
setup. Integration/deployment tests require separate authorization.

A successful Windows library or IDL build is not a successful full build. Static
Linux payload construction/validation and live WSL2/WSLC lifecycle, Authz,
installer upgrade/uninstall, and coverage checks are additional prerequisites;
do not substitute the prototype's dynamically linked collector.

### Packaging regression: native WSLC process factory

The internal `IWSLCNativeAuditProcessFactory` crosses the service/session process
boundary. Generating its MIDL proxy is insufficient: the installed interface also
needs the existing WSL service proxy/stub registration.

`NativeAuditUnitTests::BuiltMsiRegistersNativeAuditProcessFactory` opens the built
`wsl.msi` next to the test DLL using `MSIDBOPEN_READONLY`. It derives the IID from
the compiled interface's `__uuidof`, and requires exactly one HKCR/default-value
proxy mapping to the existing CLSID in the `wslservice` component. A missing MSI
or mapping fails the test rather than skipping it. It does not install the MSI,
execute custom actions, or read/write the installed registry.

After building the MSI, open its database **read-only** (do not install it) and
query the `Registry` table:

```sql
SELECT `Root`, `Key`, `Name`, `Value`, `Component_` FROM `Registry`
WHERE `Key` = 'Interface\{62D4A9C5-B632-413D-A4DB-EBAB88E9C623}\ProxyStubClsid32'
```

Require exactly one row with `Root=0` (HKCR), an empty/default `Name`,
`Value={4EA0C6DD-E9FF-48E7-994E-13A31D10DC60}`, and
`Component_=wslservice`. That component must retain its 64-bit registry attribute.
Also require the parent interface key's default value to be
`IWSLCNativeAuditProcessFactory`. Apply this check to both x64 and ARM64 packages.
Deleting the new interface registration or pointing it at a different CLSID must
fail this regression even when proxy compilation and native unit tests succeed.
