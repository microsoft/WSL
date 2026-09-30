# WSL 1 on older Windows hardware

## Purpose and scope

Document a reproducible installation route for people retaining older PCs, using a single-machine test on 30 September 2026. This is a community test record, not a promise of support for every machine or Linux application. No WSL source-code fix was made.

WSL 2 requires Second Level Address Translation (SLAT). Microsoft specifically identifies Core 2 Duo CPUs as unsupported for WSL 2. Enabling BIOS virtualisation or Virtual Machine Platform does not supply missing CPU capabilities. WSL 1 is the route tested here.

## Recorded environment

| Item | Observed value |
| --- | --- |
| Windows | 10.0.19045.7725, x64 |
| CPU | Intel Core 2 Duo T9600 @ 2.80 GHz |
| Firmware virtualisation | True |
| SLAT | False |
| VM monitor extensions | True |
| WSL command | Windows-integrated command; `wsl --version` unsupported |
| Distribution | `Ubuntu`, WSL version `1` |
| Initial Ubuntu image | 22.04.1 LTS |
| After package upgrade | 22.04.5 LTS (Jammy Jellyfish) |
| Reported Linux release string | `4.4.0-19041-Microsoft x86_64` |

The Linux release string is recorded as displayed; it is not evidence of a native Linux kernel running under WSL 1.

## Installation sequence

These steps describe the route that worked on this machine. Check the official documentation for current downloads and requirements before repeating it. Keep Windows security protection enabled. Disabling antivirus is not an installation prerequisite.

### 1. Record CPU and Windows details

In Windows PowerShell:

```powershell
Get-CimInstance Win32_Processor |
    Select-Object Name, VirtualizationFirmwareEnabled,
        SecondLevelAddressTranslationExtensions, VMMonitorModeExtensions
cmd.exe /c ver
wsl --status
wsl --list --verbose
```

### 2. Enable WSL 1 prerequisites

In PowerShell as Administrator:

```powershell
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
```

After successful completion, save work and restart Windows. Then run:

```powershell
wsl --set-default-version 1
```

In this test, `wsl --status` continued to mention a missing WSL 2 kernel. That did not prevent WSL 1 installation or launch.

### 3. Download and install Ubuntu

The Ubuntu 22.04.5 LTS listing was unavailable in both the Store website and Store app for this user. The cause of that availability restriction was not established.

Microsoft's documented direct download succeeded. In PowerShell under the intended Windows user account:

```powershell
curl.exe -L --fail "https://aka.ms/wslubuntu2204" -o "$env:USERPROFILE\Ubuntu2204.AppxBundle"
$LASTEXITCODE
```

Continue only if the exit code is `0`. The observed package download was approximately 1,065 MiB. Then run:

```powershell
Add-AppxPackage -Path "$env:USERPROFILE\Ubuntu2204.AppxBundle"
```

The downloaded package registered as `CanonicalGroupLimited.Ubuntu`, version `2204.1.7.0`, with status `Ok`. It contained the older 22.04.1 image; subsequent package upgrades produced 22.04.5.

### 4. Find and launch the registered application

`ubuntu2204.exe` was not recognised in this test. Inspect the actual registration:

```powershell
Get-AppxPackage *Ubuntu* |
    Select-Object Name, Status, PackageFullName, InstallLocation
Get-StartApps | Where-Object { $_.Name -match 'Ubuntu' }
```

Open Ubuntu from Start. The AppID observed in this test also worked with:

```powershell
explorer.exe "shell:AppsFolder\CanonicalGroupLimited.Ubuntu_79rhkp1fndgsc!ubuntu"
```

Use the AppID actually returned on the target machine if it differs. Complete first-run setup and create a Linux username and password. Password input is invisible and must not be included in public reports.

### 5. Verify WSL version

In PowerShell:

```powershell
wsl --list --verbose
```

Observed result after exiting Ubuntu:

```text
  NAME      STATE           VERSION
* Ubuntu    Stopped         1
```

`Stopped` describes the distribution's current runtime state, not an installation failure.

### 6. Upgrade and audit packages

Launch from PowerShell:

```powershell
wsl -d Ubuntu
```

Inside Ubuntu:

```bash
cd ~
sudo apt update
sudo apt upgrade
cat /etc/os-release
sudo dpkg --audit
sudo apt-get check
```

Review APT's proposed changes before accepting. The initial upgrade download failed with HTTP 400 for `tcpdump` and `libglvnd0`; repeating `sudo apt upgrade` completed. Do not generalise retrying to unrelated package errors.

## Results and remaining observations

| Check or observation | Result |
| --- | --- |
| Ubuntu registration, account creation and shell launch | Passed |
| Repository access and package upgrade | Completed after download retry |
| OS identification | Ubuntu 22.04.5 LTS |
| `sudo dpkg --audit` | No findings |
| `sudo apt-get check` | Completed without dependency errors |
| First-launch retry message | Appeared before successful setup; cause unconfirmed |
| D-Bus socket and initscript/runlevel warnings | Recorded during upgrade; not independently resolved |
| Package-list check at 9% | User reported over 15 minutes; eventually completed |
| Defender real-time scanning | User temporarily disabled it; timing and performance effect unmeasured |
| Defender restored | Requested; not yet confirmed in the test record |

Clean package checks do not prove that every background service or application works. No machine-ID regeneration was performed as part of this procedure. No controlled benchmark or performance trace was collected. Do not attribute the delay to Defender without measurements.

## Contribution boundaries

The original issue mixed WSL launch, Docker and console symptoms. This investigation does not reproduce every 2021 symptom, establish the original root cause, or validate Docker. It verifies a WSL 1 installation route on one current Windows installation.

Keep any follow-up factual, link to the original issue, and ask maintainers where clearer hardware-detection or documentation feedback belongs. For a new reproducible defect, consult current reporting guidance and collect diagnostics appropriate to that defect. Inspect logs before publication and remove passwords, tokens and unnecessary personal information.

## Official references

- [Microsoft WSL troubleshooting and SLAT requirement](https://learn.microsoft.com/en-us/windows/wsl/troubleshooting)
- [Microsoft manual installation and direct downloads](https://learn.microsoft.com/en-us/windows/wsl/install-manual)
- [Comparing WSL versions](https://learn.microsoft.com/en-us/windows/wsl/compare-versions)
- [Current WSL contribution guidance](https://github.com/microsoft/WSL/blob/master/CONTRIBUTING.md)
- [Original issue #6808](https://github.com/microsoft/WSL/issues/6808)
