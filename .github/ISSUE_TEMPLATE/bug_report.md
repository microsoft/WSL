---
name: Bug report
about: Report a bug on Windows Subsystem for Linux
title: ''
labels: ''
assignees: ''

---

Please use the following bug reporting template to help produce issues which are actionable and reproducible, including **all** command-line steps necessary to induce the failure condition. See [our contributing instructions](https://github.com/Microsoft/WSL/blob/master/CONTRIBUTING.md) for more information. If this is a console issue (a problem with layout, rendering, colors, etc.), please post to the [terminal tracker](https://github.com/microsoft/terminal/issues). For documentation improvements, please post to the [documentation tracker](https://github.com/MicrosoftDocs/WSL/issues).

**Important: Do not open GitHub issues for Windows crashes (BSODs) or security issues.**  Please direct all Windows crashes and security issues to secure@microsoft.com.  Ideally, please [configure your machine to capture minidumps](https://support.microsoft.com/en-us/help/315263/how-to-read-the-small-memory-dump-file-that-is-created-by-windows-if-a), repro the issue, and send the minidump from "C:\Windows\minidump\".
<br/><br/>

Please fill out **all** the fields! Issues with missing or incomplete issue templates will be closed. 

* Your Windows build number (run `ver` at a Windows Command Prompt), your distribution (f.e. on Debian/Ubuntu run `lsb_release -r`), and whether the issue manifests on WSL 2 and/or WSL 1 (`cat /proc/version`):

* What you're doing and what's happening. Copy&paste the _full_ set of _specific_ command-line steps necessary to reproduce the behavior, and their output. Include screenshots if that helps demonstrate the problem:

* What's wrong / what should be happening instead:

* An [`strace(1)`](https://www.man7.org/linux/man-pages/man1/strace.1.html) log of the failing command (if `some_command` is failing, then run `strace -o some_command.strace -f some_command some_args`, and link the contents of `some_command.strace` in a [gist](https://gist.github.com/)). For WSL launch issues, please [collect detailed logs](https://github.com/Microsoft/WSL/blob/master/CONTRIBUTING.md#8-detailed-logs):

