# Experimental legacy upgrade protection

Investigation for https://github.com/microsoft/WSL/issues/41529.

This branch contains an **opt-in prototype, not a production fix**. The existing
uninstall action in #40625 cannot protect upgrades from a package that predates
that action. The prototype disables WSLService activation before the packaged
updater invokes the old MSI, then restores the prior start type if the service
is still disabled when the operation finishes. It deliberately closes all
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
denial, and releasing service handles before installation.

The instrumented VM baseline is 2.7.12 with working Ubuntu. Start three clients
repeatedly launching `wsl -d Ubuntu -u root --exec /bin/sleep 1`, then deploy the
official 2.7.13 MSIX. Compare with temporary activation suppression before MSIX
deployment. Preserve verbose MSI logs, pending renames and both runtime VHDs
before and after a normal reboot. A script-level intervention tests the proposed
mechanism; it does not validate the compiled updater or its deployment timing.

## Recorded results (2026-09-07)

* Unprotected run C: automatic MSI returned 3010; both runtime VHDs were
  pending deletion before reboot and missing afterward; WSL reported
  `Wsl/Service/CreateInstance/CreateVm/HCS/ERROR_FILE_NOT_FOUND`.
* Script-guarded run D, same 2.7.12 baseline and three bounded clients: MSI
  returned 0. Pending renames contained only an MSI rollback file, not either
  runtime VHD. Version and Ubuntu probes exited 0 without timeout before and
  after reboot; system.vhd, modules.vhd and kernel all remained present.
* The standalone MSVC contract tests pass. Omitting activation suppression
  makes them fail; restoring it makes them pass again.
* One run in each condition is not a deterministic reproduction rate. The
  script disables activation before MSIX deployment, earlier than the C++
  prototype. The full WSL build and compiled-updater VM test are not complete.

## Unresolved before an upstream fix

* Process termination/power loss can bypass the C++ destructor and leave the
  service disabled. Crash recovery needs a durable design before enabling this.
* Concurrent installer processes may interfere with restoration. Per-process
  LaunchInstall serialization is not sufficient evidence of cross-process safety.
* Verify rollback, replacement service identity/configuration, and restoration
  failure. A destructor only logs restoration errors.
* The prototype covers the packaged automatic updater, not direct MSI execution
  or all `wsl --update` paths.
* Repeat from a clean baseline without interactive WSL calls and verify the built
  updater across reboot. The original failure was one full instrumented run.

Do not deploy this prototype to the host or claim #41529 fixed on these unit
tests alone.
