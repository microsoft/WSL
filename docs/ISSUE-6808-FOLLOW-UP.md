# Draft follow-up for microsoft/WSL#6808

Status: draft for review; not posted. Test date: 30 September 2026.

---

I am returning to this report with a clearer hardware assessment and new test results. My aim is to help users who still rely on older Windows PCs.

On my Intel Core 2 Duo T9600 machine running Windows 10 x64 build 19045.7725, PowerShell reports firmware virtualisation enabled, VM monitor extensions available, and SLAT unavailable. I understand that WSL 2 requires SLAT, so enabling BIOS virtualisation cannot make WSL 2 work on this CPU.

I have now successfully installed and launched Ubuntu using WSL 1:

- Set the default with `wsl --set-default-version 1`.
- Used Microsoft's documented Ubuntu 22.04 direct package download because the Store listing was unavailable in my market.
- Installed the package and launched its registered Ubuntu application.
- Created a Linux account and confirmed `wsl --list --verbose` reports version `1`.
- Upgraded the initial Ubuntu 22.04.1 image to 22.04.5 LTS.
- Confirmed `sudo dpkg --audit` returned no findings and `sudo apt-get check` completed without dependency errors.

I also observed a missing-WSL-2-kernel message in `wsl --status` after selecting WSL 1, a first-launch retry message before successful setup, and D-Bus/initscript warnings during package upgrades. Two package downloads initially returned HTTP 400; a retry succeeded. The dependency check paused at 9% for over 15 minutes according to my observation, then completed. I temporarily disabled Defender real-time scanning during this work, so these observations should not be treated as a controlled performance benchmark or evidence that Defender caused the delay.

These results demonstrate basic WSL 1 operation on this machine. They do not establish the root cause of all symptoms in my original report, validate Docker, or demonstrate WSL 2 support.

Would a focused documentation contribution explaining how to distinguish missing SLAT from disabled BIOS virtualisation, and directing applicable older systems to WSL 1, be useful? I am willing to provide a focused reproduction and current diagnostics for any remaining behaviour that warrants investigation.
