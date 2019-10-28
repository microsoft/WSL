This repo is for the reporting of issues found within and when using Windows Subsystem for Linux. Please read [CONTRIBUTING.md](https://github.com/Microsoft/WSL/blob/master/CONTRIBUTING.md) before making an issue submission. 

- Do not open Github issues for Windows crashes (BSODs) or security issues. Please direct all Windows crashes and security issues to secure@microsoft.com. Issues with security vulnerabilities may be edited to hide the vulnerability details.

## Reporting issues in Windows Console or WSL text rendering/user experience
Note that WSL distro's launch in the Windows Console (unless you have taken steps to launch a 3rd party console/terminal). Therefore, *please file UI/UX related issues in the [Windows Console issue tracker](https://github.com/microsoft/console)*. 

## Labels:

This is your best ways to interact directly with the Windows Subsystem for Linux teams. We will be monitoring and responding to issues as best we can. Please attempt to avoid filing duplicates of open or closed items when possible. Issues may be tagged with with the following labels:

- **bug** – The issue considered to be a bug internally by the dev team. This tag is generally for gaps in implemented (read: intended to be working) WSL behavior. Things marked with **bug** have a corresponding bug in on Microsoft’s internal bug tracking system. Example: "du -h reports wrong file size on DrvFs" [(#1894)](https://github.com/microsoft/WSL/issues/1894)

- **feature** – Denotes something understood as not working and is not yet implemented. Example: "Cuda can not be installed" [(#327)](https:/github.com/microsoft/WSL/issues/327)

- **fixinbound** / **fixedinNNNN** – The bug or feature request originally submitted has been addressed in whole or in part. Related or ongoing bug or feature gaps should be opened as a new issue submission if one does not already exist.

- **duplicate** – The submission is substantially duplicative of an existing issue, and/or has the same underlying cause.

- **need-repro** – The issue submission is missing fields from the issue [template](https://github.com/Microsoft/WSL/blob/master/ISSUE_TEMPLATE.md), cannot be reproduced with the information provided, or is not actionable.

- **discussion** / **question** – Submissions which are not a bug report or feature request. Example: Windows Subsystem for Linux is not open source [(#178)](https://github.com/Microsoft/WSL/issues/178)

- **bydesign** / **linux-behavior** – Denotes that an issue that is considered working as intended or would behave analogously on a native Linux kernel.

- **console** – The submission should be directed to the [console issue tracker](https://github.com/microsoft/console/issues).

- **documentation** – The submission should be directed to the [WSL documentation issue tracker](https://github.com/MicrosoftDocs/WSL).

- **wsl2** - The issue relates specifically to WSL 2.

- **fixed-in-wsl2** - The issue could be resolved by switching the distro to use the WSL 2 architecture.

Additional tags may be used to denote specific types of issues.

## Closing:

Issues may be closed by the original poster at any time.  We will close issues if:
- The issue is not a bug or feature request
- The issue has been addressed
- The issue is a duplicate of another issue
- Discussions or questions that have ran their course

### Microsoft Links:

- [Microsoft Docs](https://docs.microsoft.com/en-us/windows/wsl/about)
- [Release Notes](https://docs.microsoft.com/en-us/windows/wsl/release-notes)
- [WSL Blog](https://blogs.msdn.microsoft.com/wsl) (Historical)
- [Command Line Blog](https://blogs.msdn.microsoft.com/commandline/) (Active)

### Community Links:

- Stack Overflow: https://stackoverflow.com/questions/tagged/wsl
- Ask Ubuntu: https://askubuntu.com/questions/tagged/wsl
- reddit: https://www.reddit.com/r/bashonubuntuonwindows
- List of programs that work and don't work
    - https://github.com/ethanhs/WSL-Programs
    - https://github.com/davatron5000/can-i-subsystem-it
- Awesome WSL: https://github.com/sirredbeard/Awesome-WSL
- Tips and guides for new bash users: https://github.com/abergs/ubuntuonwindows

### Troubleshooting:

Common troubleshooting issues and solutions are available on our [MSDN documentation](https://msdn.microsoft.com/en-us/commandline/wsl/troubleshooting).
