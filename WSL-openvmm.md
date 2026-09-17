# OpenVMM WSL implementation tracker

## Current-stack assessment (2026-09-14)

This assessment compares each local branch with the branch below it, starting at `master` (`4bfbacae`). The original audit covered backend tip `13daf737`; the snapshot below includes the local rebase, committed PR 3 and PR 5 follow-ups, and uncommitted PR 6 RPC work. The PR numbers below are proposed work packages, not existing GitHub PR numbers.

| Layer | Actual branch and tip | Work present |
|---|---|---|
| refactor | `user/damanmmulye/wsl-openvmm-refactor` at `b956db18` | `IWslCoreVm`, HCS implementation adaptation, session/interface plumbing, guest connection entry point, accepted ownership comments, and lifecycle regression. |
| rpc | `user/damanmulye/wsl-openvmm-rpc` at `dd6dc8ed` plus uncommitted C2 follow-up | Rust DLL/FFI, VM/resource RPCs, bounded AF_UNIX/gRPC calls, fail-closed recovery, cancellation, error categories, and transport regressions. |
| backend | `user/damanmmulye/wsl-openvmm-backend` at `b8b146cf` | WSL backend selection, process/VM lifecycle, guest transport wiring, VirtioFS, initial networking, console logging, private RPC socket and gated packaging; accepted selection/rollback policy and selection regressions. |

**Legend:** `[x] Implemented` means the scoped WSL implementation is present in source, not that it has been built, run, merged, or approved for release. `[ ] Partial` means useful work exists but the bullet still has a gap or an unresolved design deviation. `[ ] TODO` means the requested outcome is not evidenced by this stack. `[ ] External` means completion must be established outside this WSL stack; it does not mean work in OpenVMM or offline design discussions has not happened.

**Scope totals:** 14 implemented, 10 partial, 19 TODO, 7 external (50 unique bullets). Split validation bullets are counted once; G4 is partial overall because coverage is limited to selected mock/transport and early configuration-failure cases. The implemented bullets are **A3, B1, B2, B3, B4, B5, B6, B7, C1, C2, C5, C7, D1, and D4**. These totals include the accepted PR 3 and PR 5 decisions and the PR 6 RPC-layer closeout for C2/C7; backend follow-ups remain explicitly tracked below.

The stack does not add WSLC backend call-site integration: the separate WSLC prototype from the earlier summary is not credited as completed work here. PR 3 adds a Windows lifecycle regression; PR 5 adds selection regressions and policy documentation; the uncommitted PR 6 follow-up expands the Rust transport coverage. End-to-end results, baselines, and rollout decisions cannot be inferred from a commit named "boot successful".

**Important differences from the original plan:**

- **Accepted PR 3/PR 5 design:** `IWslCoreVm` is the service-facing backend contract. `WslCoreVm` remains the HCS implementation; OpenVMM is a sibling implementation, not a backend underneath a shared `WslCoreVm` facade. This explicitly replaces the original lower-level extraction in A3/B1/B2, rather than claiming that extraction happened. The dedicated factory was removed by `2caf8dba`; `LxssUserSessionImpl::_CreateVm()` is accepted as B3's centralized creation/selection point. A2's full lifecycle contract remains separate follow-up work.
- **Accepted PR 5 policy:** HCS is the default; explicit OpenVMM opt-in fails rather than silently falling back when unavailable or when initialization fails. Rollback is manual: disable the setting and shut down WSL. A live VM retains its recorded backend until shutdown.
- **Accepted PR 6 contract:** Keep gRPC over AF_UNIX, not ttrpc. Replace transparent reconciliation with fail-closed recovery after uncertain mutations; teardown and fresh-process recreation are required.
- **C2/C7 closeout:** Close these bullets for the accepted RPC-layer scope: bounded, fail-closed RPC behavior and ordinary debugger-output tracing macros. Backend cancellation/lifetime integration, recreation-path coverage, and broader service/process diagnostics remain follow-ups, not claims of completed end-to-end integration.
- Mixed admin/non-admin access is **not implemented**: `InitializeDrvFs` and `AddVirtioFsShare` reject elevation different from the VM creator. Pass-through disks are also explicitly unsupported.
- Memory remains capped at 4 GiB. GUI/GPU, debug shell, and DNS tunneling are disabled in this backend configuration; pmem and virtio-rng configuration are not wired through the new RPC builder.
- Console/dmesg capture is implemented, but it is not kernel-panic extraction or a saved-state/crash-artifact collection pipeline.

### Source evidence

S1-S10 paths and line numbers refer to the original audited tips; S11-S13 identify committed follow-ups; S14 identifies uncommitted RPC diagnostics. Source IDs identify implementation evidence, not successful runtime results.

| ID | Evidence |
|---|---|
| S1 | `src\windows\service\exe\IWslCoreVm.h:8-88`; `src\windows\service\exe\WslCoreVm.h:43` (`WslCoreVm : IWslCoreVm`). Refactor commit `2caf8dba` removes the dedicated factory. |
| S2 | `src\windows\service\exe\LxssUserSession.cpp:2999-3028` (inline selection and failure cleanup), `:2211-2239` (backend-specific force termination); `src\windows\common\WslCoreConfig.h:298,388` (opt-in key/default); `CMakeLists.txt:44` (build gate defaults off). |
| S3 | `src\shared\inc\SocketChannel.h:620-749` (AF_UNIX I/O); `src\windows\service\exe\LxssCreateProcess.h:54,76-111`; `src\windows\service\exe\WslCoreInstance.cpp:38-50,244-247,439-442,550-580`; `src\windows\service\exe\OpenVmmWslCoreVm.cpp:38-101` (guest bridge). |
| S4 | `src\windows\wslopenvmm\src\af_unix.rs:14-45` (connect retries/timeouts); `src\windows\wslopenvmm\src\client.rs:43-79,363-383` (Tonic client, deadlines, HRESULT mapping); `src\windows\service\exe\OpenVmmWslCoreVm.cpp:459-462` (`transport=grpc`). |
| S5 | `src\windows\service\exe\OpenVmmWslCoreVm.cpp:117-187,207-256,540-586,984-1026,1335-1392` (launch, cleanup, teardown/quit, process wait and callbacks). |
| S6 | `src\windows\service\exe\OpenVmmWslCoreVm.cpp:258-350,464-538` (configuration overrides, 4-GiB cap, boot/device setup); `src\windows\wslopenvmm\src\client.rs:81-172` (configuration builder). |
| S7 | `src\windows\service\exe\VirtioFsShareRequest.cpp:8-68`; `src\windows\service\exe\OpenVmmWslCoreVm.cpp:653-768,1247-1263,1393-1406` (share requests, worker, elevation restriction); `:1035-1039` (pass-through rejection); `src\windows\wslopenvmm\src\client.rs:223-275` (share RPCs). |
| S8 | `src\windows\service\exe\OpenVmmWslCoreVm.cpp:515-521,807-875` (consomme NIC, DHCP, IPv6 enabled, port tracker); `src\windows\wslopenvmm\src\client.rs:313-349` (IPv4/IPv6 localhost port requests). |
| S9 | `src\windows\service\exe\OpenVmmWslCoreVm.cpp:342,386-430,554-577,984-1012` (dump-count override, collector/debug console, stdout/stderr log, process-exit trace); `src\windows\common\Dmesg.cpp:45-78,156-211` (pipe access and raw guest-log capture, not panic parsing). |
| S10 | `src\windows\wslopenvmm\src\client\tests.rs:86-110,133-207`: TCP-loopback mock server; port protocol/address-family assertions and share add/remove failure/retry assertions. No real VM or AF_UNIX connection/recovery test. |
| S11 | PR 3 commit `b956db18` on the refactor branch: contract comments in `src\windows\service\exe\IWslCoreVm.h` and `WslCoreVm.h`; `SimpleTests::VmBackendShutdownAndReconnect` in `test\windows\SimpleTests.cpp`. Execution is delegated to CI, with results not yet recorded here. |
| S12 | PR 5 commit `b8b146cf` on the backend branch: selection matrix in `doc\docs\technical-documentation\wslservice.exe.md`; parser, backend identity, failure/retry, live-configuration shutdown, and rollback cases in `test\windows\VmBackendTests.cpp`; invalid-boolean warning assertion in `UnitTests.cpp`; source registration, availability definition, and `configfile` linkage in `test\windows\CMakeLists.txt`. |
| S13 | PR 6 RPC follow-up committed as `3e729f2d`: `src\windows\wslopenvmm\src\rpc.rs`, `af_unix.rs`, `client.rs`, `lib.rs`, and `client\tests.rs`; private exports in `wslopenvmm.h`; revised contract in the local `src\windows\wslopenvmm\README.md`. Detailed coverage and remaining backend integration are in the PR 6 page. |
| S14 | Uncommitted C7 RPC diagnostics: epci2-style once-initialized debugger subscriber and stack-backed writer in `src\windows\wslopenvmm\src\diagnostics.rs`; initialization and ordinary tracing macros in `client.rs` and `rpc.rs`. Messages omit payloads and server error text. The accepted approach uses debugger output, not an ETW provider, custom event schemas, or request correlation metadata. |

## Original plan, annotated by scope bullet

Organize this as small, dependency-ordered PRs—not one PR per deliverable. The main sequence should be contracts → HCS-preserving abstraction → OpenVMM boot → filesystem/networking compatibility → diagnostics → rollout gates. Start WHP memory work in parallel; it should block removing the memory cap, not the initial capped-memory backend.

The order below maps all 50 description bullets into proposed PRs. Some validation bullets intentionally span an early foundation PR and a later completion PR.

Account for work you already have

These are foundations to consume, not features to implement again. Merged PRs do not, by themselves, establish completion of their broader deliverables.

| Area | Existing work | Planning implication |
| --- | --- | --- |
| Backend prototype | WSL #40629, open; your current WSL branch also contains backend and private AF_UNIX RPC work | Extract cohesive changes into the backend PRs below rather than starting over. |
| VirtioFS | OpenVMM #3821, WSL #41129, WSL #41151, all merged | Focus on OpenVMM integration and identity/elevation gaps, not rebuilding aggregate shares. |
| Networking | OpenVMM #2398, IPv6, merged; #4378, control/data-path separation, open | Consume existing IPv6 support and identify the remaining integration gaps. Treat the networking refactor as a dependency only where needed. |
| RPC configuration | OpenVMM #4420, open | Land the required network/filesystem RPC capabilities before their WSL consumers. |
| Crash artifacts | OpenVMM #3882, triple-fault .vmrs, merged | Extend and integrate the existing mechanism; distinguish triple faults from kernel panics and host-process crashes. |

At the initial ADO lookup, all seven deliverables said Proposed. That state is not a reliable measure of implementation progress; the checklist below records local-stack evidence separately.

Proposed PR order

References:  A1  means the first Scope bullet in deliverable A.

| Key | Deliverable |
| --- | --- |
| A | 62679114 — Architecture and compatibility matrix |
| B | 63428739 — Pluggable VM backends |
| C | 63428740 — OpenVMM backend |
| D | 63428744 — Filesystem and networking |
| E | 63428746 — Memory elasticity and nested virtualization |
| F | 63428745 — Crash diagnostics |
| G | 63428747 — Compatibility and regression validation |

### PR 1 — Architecture, compatibility, and ownership contract

Design/documentation PR. First; approve the relevant decisions before implementing their consumers.

- [ ] **TODO - A1:** Define the supported WSL and WSLC scenario-compatibility matrix. No matrix is added by this stack.
- [ ] **Partial - A2:** Define `IWslVmBackend` responsibilities and lifecycle contract. `IWslCoreVm` and lifecycle implementations exist (refactor/backend; S1, S5), but the approved contract must reflect the actual interface and ownership model.
- [x] **Implemented - A3:** Define the boundary between service orchestration and HCS-specific behavior. The sibling HCS/OpenVMM design is explicitly accepted and documented in PR 3 below and the interface/class comments (S1, S11). This supersedes the originally proposed split within `WslCoreVm`.
- [ ] **Partial - A4:** Define backend selection, feature control, rollback, and configuration behavior. Compile-time and `.wslconfig` gates and the accepted selection/failure/manual-rollback matrix are documented (backend; S2, S12). The broader unsupported-setting compatibility contract remains outstanding.
- [ ] **Partial - A5:** Define the guest communication abstraction for HvSocket and vsock. Code implements callbacks and the guest bridge (refactor/backend; S3); the reviewed transport/lifecycle contract is not evidenced.
- [ ] **Partial - A6:** Record the initial VirtioFS-only and consomme-only constraints. These are enforced by configuration overrides (backend; S6), but a reviewed compatibility/limitations document is still needed.
- [ ] **TODO - A7:** Identify repository, component, and DRI ownership for every gap. No ownership table is added.
- [ ] **TODO - A8:** Resolve ownership overlap between scenarios 62917985 and 61024686. No recorded resolution is evidenced by the branch changes.
- [ ] **TODO - D3:** Design elevation/broker behavior for pass-through disk file opens. The current backend rejects non-VHD disks and does not implement a pass-through broker (backend; S7).
- [ ] **TODO - D6:** Define the migration path to converged WSL networking. Forcing consomme in configuration is not a migration plan (S6).
- [ ] **TODO - F5:** Define artifact retention, size, privacy, and upload behavior. Socket/pipe ACLs and local logging exist, but no artifact policy is added (S9).

Keep this focused on decisions, not implementations. In particular, define when fallback is allowed; do not leave “safe fallback” to become an arbitrary retry after a partially created VM.

### PR 2 — Backend comparison harness and baseline measurements

WSL test/infrastructure PR. Start after PR 1; develop alongside the implementation.

- [ ] **TODO - G1:** Create the end-to-end matrix for WSL and WSLC on HCS and OpenVMM. Mock RPC tests are not a backend matrix (S10).
- [ ] **TODO - G5, foundation:** Establish measurement tooling and HCS startup, memory, CPU, I/O, networking, and reliability baselines; collect OpenVMM results once available. No benchmark harness or baseline results are added.
- [ ] **TODO - G7, definition:** Agree preview/GA pass rates and regression thresholds before deciding whether results are acceptable. No threshold definitions are added.

This is infrastructure, not a reason to defer feature-specific tests until the end.

### PR 3 — Isolate the HCS backend behind the accepted service contract

WSL PR. Depends on PR 1.

- [x] **Implemented - B1 (accepted revised scope):** Isolate HCS-specific VM operations from service callers behind `IWslCoreVm`, retaining HCS ownership in `WslCoreVm` (S1, S11).
- [x] **Implemented - B2 (accepted revised scope):** Put existing HCS behavior behind the service-facing backend contract; use sibling HCS/OpenVMM implementations rather than a shared facade (S1, S11).
- [x] **Implemented - B6:** Preserve HCS initialization, networking, VirtioFS, shutdown, and error telemetry, with regression coverage (S11).

Keep OpenVMM implementation out of this PR. Its review question should be: does the abstraction preserve HCS behavior?

[PR 3 details: ownership, test coverage, CI sign-off, and build evidence](WSL-openvmm/PR-3.md).

### PR 4 — Make guest control channels transport-neutral

WSL PR. Depends on the agreed transport contract and PR 3.

- [x] **Implemented - B5:** Abstract guest control channels away from the HvSocket-specific implementation. `ConnectToGuest` and connector callbacks are wired through instance/process/session creation; `SocketChannel` handles AF_UNIX separately from existing Windows I/O (refactor/backend; S3).

Introduce and exercise the abstraction with existing behavior first. Do not conflate the host-side gRPC socket with the guest-control transport; they are separate contracts.

### PR 5 — Backend selection, fail-fast behavior, and manual rollback

WSL PR. Depends on PRs 3–4.

- [x] **Implemented - B3 (accepted revised scope):** Centralize WSL VM creation/selection in `_CreateVm()` with `IWslCoreVm` callers; no separate factory is required (S1, S2, S12).
- [x] **Implemented - B4 (accepted policy):** Keep HCS as default, OpenVMM explicitly opt-in, failures explicit, and rollback manual after shutdown (S2, S12).
- [x] **Implemented - B7:** Add configuration-parsing and integration coverage for backend selection, failure handling, shutdown, and rollback (S12).

[PR 5 details: accepted policy, test cases, CI requirements, and build evidence](WSL-openvmm/PR-5.md).

### PR 6 — OpenVMM process supervision and gRPC client

WSL PR. Depends on the contracts and shared abstractions.

- [x] **Implemented - C1:** Implement process launch, lifetime, and termination handling. User-token launch, kill-on-close job, process registry/wait, cleanup, timeout-based forced termination, and exit callbacks are present (backend; S5).
- [x] **Implemented - C2 (accepted RPC-layer scope):** Bounded gRPC/AF_UNIX calls, fail-closed status handling, cancellation, and HRESULT mapping are implemented with client-owned per-handle synchronization (rpc; S13). Closed for this scope; backend lifecycle integration remains a separate follow-up.
- [x] **Implemented - C7 (accepted logging scope):** Existing backend traces, distinct RPC error categories, and ordinary tracing macros through the once-initialized debugger subscriber are implemented (S3, S5, S7, S9, S13, S14). No ETW provider, structured event schema, or correlation metadata is required for closeout.
- [ ] **Partial - G4, transport portion:** AF_UNIX startup, deadlines, cancellation races, fail-closed status, and silent/wrong-protocol peers have regression coverage (rpc; S13). Remaining: actual process crashes, lost-response resource state, and service shutdown races.

**Backend follow-ups retained outside the C2/C7 closeout:**

- [ ] Wire cancellation into process exit/termination, coordinate RPC-handle lifetime, and cover teardown followed by fresh-process recovery through service call sites.
- [ ] Integrate service/process diagnostics and define the backend-wide failure taxonomy.

Your current AF_UNIX work belongs here. Distinguish establishing/re-establishing a connection from replaying a VM-management operation whose outcome is unknown.

[PR 6 details: revised RPC contract, regression coverage, and remaining backend integration](WSL-openvmm/PR-6.md).

### PR 7 — Configure, boot, and manage an OpenVMM VM

WSL PR. Depends on PRs 4–6 and the required upstream RPC/device capabilities.

- [ ] **Partial - C3:** Translate WSL VM settings into OpenVMM configuration. Kernel/initrd/modules, command line, CPU, capped memory, disks and NIC are translated (rpc/backend; S6). Several settings are forcibly disabled/overridden; complete or explicitly approve the supported-setting matrix.
- [ ] **Partial - C4:** Configure boot, memory, processors, serial, vsock, disks, pmem, and virtio-rng. Boot/CPU/memory/serial/virtio-console/vsock/SCSI configuration exists (rpc/backend; S6); pmem and virtio-rng are not configured by the new builder. Sending boot entropy is not virtio-rng support.
- [x] **Implemented - C5:** Implement start, stop, shutdown, terminate, and unexpected-exit handling. Create/resume, channel shutdown, teardown/quit, timed force termination, process-exit signaling and session callback routing are wired (rpc/backend; S2, S5). Runtime reliability coverage is tracked separately in G2/G4.
- [ ] **TODO - G2, initial slice:** Automate boot, distro launch, basic disk, vsock, console, and shutdown scenarios. The stack contains implementation and mock RPC tests, not real-VM scenario automation (S10).
- [ ] **Partial - G4, configuration portion:** Add malformed-configuration negative tests. PR 5 adds invalid backend-boolean parsing/warnings and early unsupported-system-distro failure/retry coverage (S12). Broader malformed VM/RPC configuration and post-allocation failure cases remain outstanding (S6, S10).

This is the first usable, gated backend milestone, initially retaining the memory cap. Consume already-implemented boot/RPC functionality rather than duplicating it.

### PR 8 — VirtioFS integration and mixed-elevation access

WSL integration PR. Depends on PR 7 and upstream filesystem capabilities.

- [x] **Implemented - D1:** Implement VirtioFS-based cross-OS filesystem access. Share request/response handling, guest listener/worker, canonical host paths, read-only options, and VPCI share RPCs are connected (rpc/backend; S7). This is creator-elevation access; D2 remains separate.
- [ ] **TODO - D2:** Support admin and non-admin Windows file access from the same VM. `AddVirtioFsShare` rejects `Admin != m_creatorElevated`; `InitializeDrvFs` explicitly rejects switching elevation context after creation (backend; S7). The guard is a limitation, not mixed-elevation support.

Apply the broker/identity decisions from PR 1. If additional OpenVMM or DeviceHost mechanisms are required, land those as separate prerequisite PRs; do not bundle cross-repository implementation into this WSL PR.

### PR 9 — Consommé networking integration and compatibility

WSL integration PR. Depends on PR 7 and the required OpenVMM networking/RPC changes.

- [x] **Implemented - D4:** Integrate initial consomme networking. NIC configuration, mini_init networking/DHCP setup, port tracker and localhost bind/unbind RPCs are wired (rpc/backend; S8).
- [ ] **Partial - D5:** Consume the required consomme IPv6 changes. Guest configuration enables IPv6 and RPCs handle `AF_INET6`/`::1`, with mock field assertions (rpc/backend; S8, S10). Confirm the consumed OpenVMM version satisfies the upstream dependency and exercise real IPv6 behavior.
- [ ] **TODO - D7:** Validate DNS, localhost, VPN, proxy, firewall, IPv6, and multi-distro behavior. Configuration checks and mock port serialization are not networking compatibility results; no such matrix/automation is added.

PRs 8 and 9 can proceed independently. Existing IPv6 support is a starting point—not proof that the entire WSL networking matrix passes.

### PR 10 — Complete the OpenVMM crash-artifact producer

OpenVMM PR. Can proceed in parallel once the artifact contract is agreed.

- [ ] **External - F1:** Complete the mechanism to produce a VM saved-state or crash artifact. Earlier evidence identified merged OpenVMM #3882 for triple faults; this WSL stack neither implements nor proves the complete producer contract. Track upstream completion and consumption separately.
- [ ] **External - F2:** Add the compatible compression writer for the selected format, or update the consumer. Neither change is present in this WSL stack; verify the selected upstream format and remaining consumer work.

Build on merged #3882. If the chosen solution instead changes the consumer, place F2 in PR 11, rather than implementing both approaches.

The previously observed OpenVMM `Add crash dump path option` commit belongs to this diagnostics work, not the network/filesystem RPC story in #4420. It is outside the WSL stack assessed here.

### PR 11 — WSL/WSLC diagnostic collection and debugger integration

WSL PR. Depends on PR 7; artifact collection additionally depends on PR 10.

- [ ] **TODO - C6:** Add kernel debugger support. Debug console/early-console output is wired, but OpenVMM kernel-debugger configuration is not (backend; S6, S9). Do not count a debug console as a debugger.
- [ ] **Partial - F3:** Integrate artifact collection into WSL and WSLC diagnostics. WSL reuses `DmesgCollector`, adds user-accessible console pipes, and writes OpenVMM stdout/stderr locally (backend; S9). Saved-state/crash-artifact collection and WSLC integration are not added.
- [ ] **TODO - F4:** Extract kernel-panic details from dmesg collector output. The reused collector buffers/emits raw guest log lines; the stack adds pipe access, not panic parsing or attribution (S9).
- [ ] **TODO - F6:** Update log-collection scripts for OpenVMM logs and traces. No `diagnostics` scripts change; creating a local `.log` file is only a prerequisite.
- [ ] **Partial - F7:** Add telemetry for dump success/failure, parsing, and backend crash buckets. Process-exit code/VM-ID traces and guest logs exist (backend; S9), but dump outcome, parsing and crash-bucket telemetry are not implemented.

Bring this forward alongside filesystem/networking work: actionable diagnostics are useful before broad stress testing, not just before release.

### PR 12 — WHP memory contract and accounting design

Design/documentation PR. Start alongside PR 1, despite its position in this implementation sequence.

- [ ] **External - E1:** Confirm WHP deferred-commit and sparse-allocation requirements with the WHP owner. Owner agreement is not evidenced by WSL branch changes; attach the decision separately.
- [ ] **External - E5:** Define host-commit versus guest-visible memory accounting and telemetry. No accounting contract or new memory telemetry is present; the 4-GiB clamp is not accounting (S6).

Owner agreement is a prerequisite, not something a code PR alone can accomplish. Explicitly determine whether host/WHP changes are required; ballooning alone should not be assumed to solve upfront host commit.

### PR 13 — Virtio-balloon support

OpenVMM PR. Depends on PR 12.

- [ ] **External - E2:** Implement the virtio-balloon support required by WSL and WSLC. No balloon configuration/control integration is added in this WSL stack (S6). Track separate OpenVMM implementation and its WSL/WSLC consumption.

Keep this independently reviewable from cold-discard and nested virtualization. Any WSL policy/configuration wiring should be a separate consuming PR if it requires code changes there.

### PR 14 — Cold-discard support and memory-elasticity integration

OpenVMM PR, followed by a WSL integration PR where needed. Depends on PRs 12–13.

- [ ] **External - E3:** Implement qemu-style cold-discard hints or the approved equivalent. No new host memory-discard integration is present. Existing guest reclaim settings and disk trim commands are not evidence of this host-memory feature.
- [ ] **TODO - E4:** Validate grow, shrink, reclaim, pressure, suspend, and multi-VM behavior. No elasticity scenario coverage/results are added; the memory cap remains (S6).

Do not remove the WSL memory cap merely because the device exists. Removal should follow demonstrated host-commit and reclaim behavior plus the memory stress/performance coverage below.

### PR 15 — Remaining nested-virtualization support

OpenVMM/WHP-owned implementation PRs. Independent of balloon/discard unless a concrete shared dependency emerges.

- [ ] **External - E6:** Complete the remaining nested-virtualization work. The new WSL RPC configuration does not wire a nested-virtualization setting (S6); track OpenVMM/WHP completion and explicit WSL consumption separately.

Keep this a separate workstream. The deliverable groups nesting with memory, but its description does not establish that they must form one linear code stack.

### PR 16 — Complete compatibility automation and cross-feature stress

Test PRs in the repository owning each harness. Depends on the applicable feature PRs.

- [ ] **TODO - G2, completion:** Complete automated disk, VirtioFS, vsock, console, networking, boot, launch, and shutdown coverage. Two mock RPC tests do not exercise these real-VM scenarios (S10).
- [ ] **TODO - G3:** Add multi-distro, repeated attach/detach, restart, update, and hot-add stress. Share retry assertions are not repeated real-device or multi-VM stress.
- [ ] **TODO - G4, completion:** Add host-resource-pressure tests and complete cross-feature failure coverage. Only the narrow mock-RPC portion in PR 6 is present (S10).
- [ ] **TODO - E7:** Add memory-elasticity and nested-workload stress/performance coverage. No such harness/results are added.

The feature PRs should already carry their focused tests. This layer covers interactions, longer-running workloads, and the full matrix.

### PR 17 — Enforce performance and rollout gates

WSL validation/release-infrastructure PR. Depends on representative results from the preceding work.

- [ ] **TODO - G5, completion:** Establish the comparable OpenVMM baselines across startup, memory, CPU, I/O, networking, and reliability. Slow-operation logging does not supply comparative baseline results.
- [ ] **TODO - G6:** Use WSLC startup-time P95 measure 63134665 as a rollout signal. No measure integration is added.
- [ ] **TODO - G7, enforcement:** Make the agreed preview/GA thresholds enforceable gates. Compile-time and config opt-in gates are not health/performance release gates.

Do not invent numerical thresholds from the work-item text; it specifies that they must be defined, not what their values are.

## Stack boundaries and dependencies outside your seven items

Use a short WSL foundation stack for PRs 3–7, then separate filesystem, networking, diagnostics, and memory workstreams. OpenVMM prerequisite PRs belong in OpenVMM stacks; connect them to WSL consumers through explicit dependency links and consumed versions—not one cross-repository branch chain.

Three sibling deliverables need to remain visible in the dependency map:

| Dependency | Where it matters |
| --- | --- |
| 63428742 — Guest channels and virtio devices | PRs 4, 7, and 8 require the selected mini_init transport, independent control/diagnostic channels, and appropriate VirtioFS/device support. This is a real dependency omitted from the seven-item list. |
| 62679000 — Productization and release pipeline | Required to consume supported, versioned OpenVMM artifacts and ship the result; avoid making prototype completion synonymous with release readiness. |
| 63428748 — Preview rollout and GA readiness | Owns rollout execution. PRs 5 and 17 should provide selection controls and gates without duplicating its rollout ownership. |

Also align PRs 6 and 11 with 63459355 — Diagnosability. GPU support is explicitly non-blocking in the parent scenario and should not hold up this core sequence.
