This repo is for the reporting of issues found within and when using Windows Subsystem for Linux.
- Do not open Github issues for Windows crashes (BSODs) or security issues. Please direct all Windows crashes and security issues to secure@microsoft.com. Issues with security vulnerabilities may be edited to hide the vulnerability details.

## Reporting issues in Windows Console or WSL text rendering/user experience
Note that WSL distro's launch in the Windows Console (unless you have taken steps to launch a 3rd party console/terminal). Therefore, *please file UI/UX related issues in the [Windows Console issue tracker](https://github.com/microsoft/console)*.

### Labels:

This and our [User Voice page](https://wpdev.uservoice.com/forums/266908-command-prompt-console-bash-on-ubuntu-on-windo/category/161892-bash) are your best ways to interact directly with the Windows Subsystem for Linux teams. We will be monitoring and responding to issues as best we can. Please attempt to avoid filing duplicates of open or closed items when possible. In the spirit of openness we will be tagging issues with the following:

- **bug** – We consider this issue to be a bug internally. This tag is generally for bugs in implemented features, or something we consider to be a “bug level” change. Things marked with Bug have a corresponding bug in on Microsoft’s internal bug tracking system.
  - Example: No internet connectivity in Bash [(#5)](https://github.com/Microsoft/WSL/issues/5)

- **feature** – Denotes something that is not yet implemented.  The community should use our [User Voice](https://wpdev.uservoice.com/forums/266908-command-prompt-console-bash-on-ubuntu-on-windo/category/161892-bash) page for voting on which features everyone feels as the most important.  The team will take the User Voice page as input in deciding what to work on next.
  - Example:  Docker is not working [(#85)](https://github.com/Microsoft/WSL/issues/85)

- **discussion** – Denotes a discussion on the board that does not relate to a specific feature.
  - Example: Windows Subsystem for Linux is not open source [(#178)](https://github.com/Microsoft/WSL/issues/178)

- **fixinbound** – When possible, we will mark bugs that have been fixed internally.  Unfortunately we cannot say specifically when the bug will hit the insider flights.

- **bydesign** – Denotes that an issue is raised that we consider is working as intended.  We will give some reasoning why this is by design.  After one week we will either close the issue or mark as Discussion depending on what comes up.

Additional tags may be used to denote specific types of issues.  These include items such as network or symlink.

### Closing:

Issues may be closed by the original poster at any time.  We will close issues if:
- One week passes after the change goes out to the Insider Fast ring
- An issue is clearly a dup of another.  The duplicate will be linked
- Any discussion that has clearly run its course

### Microsoft Links:

- [MSDN Documentation](https://msdn.microsoft.com/en-us/commandline/wsl/about)
- [Release Notes](https://msdn.microsoft.com/en-us/commandline/wsl/release_notes)
- [User Voice](https://wpdev.uservoice.com/forums/266908-command-prompt-console-bash-on-ubuntu-on-windo/category/161892-bash)
- [WSL Blog](https://blogs.msdn.microsoft.com/wsl)
- [Console Blog](https://blogs.msdn.microsoft.com/commandline/)

### Community Links:

- Stack Overflow: https://stackoverflow.com/questions/tagged/wsl
- Ask Ubuntu: https://askubuntu.com/questions/tagged/wsl
- reddit: https://www.reddit.com/r/bashonubuntuonwindows
- List of programs that work and don't work
    - https://github.com/ethanhs/WSL-Programs
    - https://github.com/davatron5000/can-i-subsystem-it
- Tips and guides for new bash users: https://github.com/abergs/ubuntuonwindows

### Troubleshooting:

Common troubleshooting issues and solutions are available on our [MSDN documentation](https://msdn.microsoft.com/en-us/commandline/wsl/troubleshooting).
