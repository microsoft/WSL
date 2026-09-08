# Experimental legacy upgrade protection

Investigation for https://github.com/microsoft/WSL/issues/41529.

See [compiled-updater evidence](evidence.md) for the controlled ON/OFF results,
selected MSI/registry observations, and the distinction from E_UNEXPECTED.

This branch contains an **opt-in prototype, not a production fix**. The existing
uninstall action in #40625 cannot protect upgrades from a package that predates
that action. The prototype disables WSLService activation before the packaged
updater invokes the old MSI, then restores the prior start type if the service
is still disabled when the operation finishes. Before disabling, it flushes a
recovery record under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Lxss\UpgradeRecovery\<service>`.
This is outside the service key that MSI deletes during upgrade. Updater service startup checks
this record even when the installed version is already current. A per-service
cross-process mutex serializes the guard and recovery; installation rechecks the
installed version after acquiring it. It deliberately closes all
service handles before MSI runs, so the service can be deleted and recreated.

Enable only in a disposable VM with `-DWSL_EXPERIMENTAL_UPGRADE_GUARD=ON`.
The default build does not activate the prototype. It does preserve automatic
upgrade logs for result 3010, which are needed to diagnose post-reboot deletion.

## Checks

The isolated contract tests compile the actual guard with simulated SCM calls.
They do not modify the host's services. With a checkout of microsoft/wil:

```powershell
cmake -S tools/test/legacy-upgrade-guard -B out/guard-tests -A x64 -DWIL_INCLUDE_DIR=<wil-checkout>/include
cmake --build out/guard-tests --config Debug
ctest --test-dir out/guard-tests -C Debug --output-on-failure
```

Covered: automatic/manual start restoration, exception unwinding, an already
disabled service, a replacement configuration, absent/deleted services, access
denial, releasing service handles before installation, persisted recovery,
failure to flush the record, invalid records, and refusing recovery while the
MSI service is executing an installation.

The instrumented VM baseline is 2.7.12 with working Ubuntu. Start three clients
repeatedly launching `wsl -d Ubuntu -u root --exec /bin/sleep 1`, then deploy the
official 2.7.13 MSIX. Compare with temporary activation suppression before MSIX
deployment. Preserve verbose MSI logs, pending renames and both runtime VHDs
before and after a normal reboot. A script-level intervention tests the proposed
mechanism; it does not validate the compiled updater or its deployment timing.

## Recorded results (2026-09-07 through 2026-09-08)

* Unprotected run C: automatic MSI returned 3010; both runtime VHDs were
  pending deletion before reboot and missing afterward; WSL reported
  `Wsl/Service/CreateInstance/CreateVm/HCS/ERROR_FILE_NOT_FOUND`.
* Script-guarded run D, same 2.7.12 baseline and three bounded clients: MSI
  returned 0. Pending renames contained only an MSI rollback file, not either
  runtime VHD. Version and Ubuntu probes exited 0 without timeout before and
  after reboot; system.vhd, modules.vhd and kernel all remained present.
* The standalone MSVC contract tests pass. Omitting activation suppression
  makes them fail; restoring it makes them pass again.
* An elevated real-SCM probe in the disposable Windows VM also passed normal
  activation suppression and restored a manual-start test service (3 -> 3).
  Terminating the probe without stack unwinding left its test service disabled
  (3 -> 4). This confirms the crash-recovery defect; it is not merely a
  hypothetical risk. The disposable services were deleted after both checks.
* With persisted recovery added, a new probe process restored that crash state
  from 4 to 3 and cleared the record. Another real-SCM test held a live guard
  while a separate recovery process waited: the service stayed disabled until
  the guard exited, then both processes completed with the original start type.
* A crash followed by a normal guest reboot preserved the disabled state and
  original-start record. A fresh recovery process restored 4 -> 3, removed the
  record and exited 0. The temporary service was deleted afterward. This was
  not a forced-power-loss test or a full MSI rollback test.
* A two-version MSI fixture deliberately fails after InstallExecute and rolls
  back service deletion/recreation. Keeping the journal in the service key
  failed: MSI returned the expected 1603 and restored the old product, but
  left the service disabled (4) with no journal. Moving the journal outside
  the service key passed the same test: the old product was restored, the new
  product was absent, and the guard restored manual start (3) and cleared the
  journal. Both fixture products were removed afterward. Normal, crash,
  concurrent recovery and crash-then-normal-reboot probes were repeated with
  the independent journal and passed; their temporary services were deleted.
* The actual `wslinstaller` target builds with MSVC 19.44 for x64 Release,
  both with the experimental option ON and OFF. On a Chinese Windows host,
  explicit `/utf-8` C/C++ flags were needed for existing UTF-8 source files.
  The toolchain requires Clang, ATL and x64/x86 Spectre runtime libraries.
* Compiled-updater run E: replace only wslinstaller.exe in the official 2.7.13
  x64 MSIX layout, sign locally with the repository's development certificate,
  and deploy from the 2.7.12 baseline with the same bounded clients. The bundled
  official MSI is unchanged. With the guard enabled, MSI returned 0 and neither
  runtime VHD was pending deletion. After reboot, both files remained and Ubuntu
  exited 0. However, before reboot Ubuntu returned -1 / Wsl/Service/E_UNEXPECTED;
  this run is not a complete success for pre-reboot availability.
* Compiled control F uses the same source and packaging procedure with the guard
  disabled. MSI returned 3010 and scheduled both runtime VHDs for deletion.
  Ubuntu worked before reboot; after reboot both VHDs were gone and Ubuntu
  reported Wsl/Service/CreateInstance/CreateVm/HCS/ERROR_FILE_NOT_FOUND. This
  strengthens the causal evidence beyond the earlier script-level intervention.
* Guarded runs G and H again returned 0 and retained both VHDs, but reproduced
  pre-reboot E_UNEXPECTED. H's trace shows client timeouts and forced termination
  preceding CoImpersonateClient failures (0x800706E5), followed by a closed
  WslCorePort during CreateSession. Restarting only WSLService restored Ubuntu
  without rebooting Windows. This was a diagnostic script intervention, not a
  change to the candidate installer. The 5-second client deadline may expose a
  separate cancellation failure; this causal link still needs a controlled test.
* One run in each condition is not a deterministic reproduction rate. The
  script disables activation before MSIX deployment, earlier than the C++
  prototype. The complete WSL distribution build has not been run. Further
  compiled-updater trials and diagnosis of pre-reboot E_UNEXPECTED remain.

## Unresolved before an upstream fix

* Validate the compiled updater's startup recovery, not only explicit recovery
  calls in scm-probe. Recovery is retried on a subsequent updater start, not by
  an independently installed watchdog.
* Recovery preserves its record and fails while msiserver cannot accept a stop.
  This status check is not atomic with a non-cooperating installer starting.
  The named mutex coordinates this updater only, not arbitrary MSI clients.
* The rollback fixture tests the guard around a real MSI transaction, but does
  not exercise the full WSL package or automatic updater startup.
* Verify restoration failures and service configuration changes by other actors.
  A destructor only logs restoration errors and leaves the journal for retry.
* The prototype covers the packaged automatic updater, not direct MSI execution
  or all `wsl --update` paths.
* Repeat from a clean baseline without interactive WSL calls and verify the built
  updater across reboot. The original failure was one full instrumented run.
* Compare longer client deadlines while preserving the original 5-second stress
  results. Do not add unconditional service restarts or claim pre-reboot
  availability fixed solely because the diagnostic restart recovered it.

Do not deploy this prototype to the host or claim #41529 fixed on these unit
tests alone.

`scm-probe` is an explicit VM-only diagnostic target, excluded from CTest. Create
a uniquely named disposable service with prefix `WslGuardProbe-`, then run
`scm-probe <service-name> normal` or `scm-probe <service-name> crash`. The latter
deliberately exits with 99 via TerminateProcess. Inspect the service start type
from a separate process and delete the test service afterward. Never substitute
WSLService or another real service.

### MSI rollback fixture

In the disposable elevated VM, use WiX 5 to build `rollback-fixture.wxs` twice,
with `Version=1.0.0` / `Version=2.0.0`, distinct `ProductCode` GUIDs, and
`ProbePath` pointing to the built scm-probe.exe. Keep the fixture's upgrade
and component GUIDs unchanged. Install the old MSI normally, then run:

```powershell
scm-probe WslGuardProbe-RollbackFixture install <new.msi>
```

The intentional failure must return 1603. Verify that the old product remains,
the new product is absent, the service Start is 3, and the recovery DWORD is
absent. The verbose transaction log is `<new.msi>.log`. Uninstall the fixture
products afterward. This fixture never starts its installed service executable;
it tests SCM configuration and MSI rollback only.
