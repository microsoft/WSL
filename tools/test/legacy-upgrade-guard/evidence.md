# Compiled updater evidence for #41529

These are selected observations from disposable Hyper-V VM runs on 2026-09-08,
not a claim of a deterministic failure rate. The original issue contains the
initial reproduction logs and screenshot.

## Setup

* Windows 11 25H2, build 26200.8037; nested virtualization; 4 vCPUs, 4 GiB RAM.
* Restore the same working WSL 2.7.12.0 + Ubuntu checkpoint for each trial.
* Build `wslinstaller` at commit `945900be3e8231d23b60febd0ffac26f3ffb35d4`,
  MSVC 19.44, x64 Release, package version 2.7.13.0, with the guard ON or OFF.
* Replace only wslinstaller.exe in the official 2.7.13 x64 MSIX layout and
  re-sign locally using the repository's development certificate workflow.
  Trust that test certificate only inside the disposable VM.
* The embedded official MSI is unchanged. SHA-256:
  `A3505A50F4CC585551D11D9DE824BA4375448D7A68F2E71D3FB315FA986FC754`.
* Three clients repeatedly run `wsl -d Ubuntu -u root --exec /bin/sleep 1`
  for 180 seconds, with a 200 ms gap. A launch taking over 5 seconds is killed
  and recorded as a timeout. Deploy the MSIX three seconds after starting them.
* Configure `Lxss\MSI\UpgradeLogFile`, capture pending renames, and run bounded
  version/Ubuntu probes before and after a normal Windows reboot. No manual
  activation suppression is used in these compiled-updater trials.

## Results

| Trial | Guard | Raw MSI result | Ubuntu before reboot | Ubuntu after reboot | VHDs after reboot |
|---|---|---|---|---|---|
| E | ON | 0 | E_UNEXPECTED | Exit 0 | Both present |
| F | OFF | 3010 | Exit 0 | ERROR_FILE_NOT_FOUND | Both absent |
| G | ON | 0 | E_UNEXPECTED | Exit 0 | Both present |
| H | ON | 0 | E_UNEXPECTED; exit 0 after diagnostic WSLService restart | Exit 0 | Both present |

H's service restart is a **test intervention**, not part of the candidate patch.
G traces the failing startup; H traces the upgrade through that startup.

## Direct evidence of delayed deletion (F)

Verbose MSI log:

```text
Info 1903.Scheduling reboot operation: Deleting file C:\Program Files\WSL\system.vhd. Must reboot to complete operation.
Info 1903.Scheduling reboot operation: Deleting file C:\Program Files\WSL\tools\modules.vhd. Must reboot to complete operation.
```

The relevant `PendingFileRenameOperations` source/destination pairs were:

```text
*1\??\C:\Program Files\WSL\system.vhd
<empty destination>
*1\??\C:\Program Files\WSL\tools\modules.vhd
<empty destination>
```

Both files existed before reboot and were absent afterward. Ubuntu then reported:

```text
Wsl/Service/CreateInstance/CreateVm/HCS/ERROR_FILE_NOT_FOUND
```

E/G/H returned raw MSI result 0 and had no pending deletion entries for these
VHDs. Their remaining pending entries referred to MSI rollback files.

## Why the pre-reboot error is not the missing-VHD failure

E/G/H's E_UNEXPECTED occurred before reboot, with both VHDs present and no
corresponding pending deletion entries. H recovered through WSLService restart
alone, without MSI repair, replacing VHDs, or first rebooting Windows.

H's decoded ETW contains CoImpersonateClient failures (`0x800706E5`) after
client timeouts/termination, followed by:

```text
Expected message LxInitMessageCreateSessionResponse, but socket WslCorePort was closed
```

This establishes a different immediate failure mechanism. It does **not** prove
complete independence from upgrade timing or the guard. Client cancellation is
a hypothesis requiring a separate controlled test; pre-reboot availability is
not claimed fixed here. That investigation is deferred rather than hidden by
adding an unconditional service restart to this patch.

## Other validation

* Actual installer builds with guard ON and OFF; standalone CTest passes.
* Real SCM tests cover activation suppression, ordinary restoration, recovery
  after process termination, concurrent recovery waiting, and normal reboot.
* Real two-version MSI fixture returns intentional 1603 and restores the old
  product. A service-key journal failed this rollback test; the independent
  journal restores manual start and clears its recovery value.
* Full distribution build, forced power loss, and automatic recovery of the
  packaged updater after a crash are not claimed tested. See README limitations.
