---
name: Bug report
about: Report a bug on Windows Subsystem for Linux
title: ''
labels: ''
assignees: ''

---
<!--
🚨🚨🚨🚨🚨🚨🚨🚨🚨🚨

I ACKNOWLEDGE THE FOLLOWING BEFORE PROCEEDING:
1. If I delete this entire template and go my own path, the core team may close my issue without further explanation or engagement.
2. If I list multiple bugs/concerns in this one issue, the core team may close my issue without further explanation or engagement.
3. If I write an issue that has many duplicates, the core team may close my issue without further explanation or engagement (and without necessarily spending time to find the exact duplicate ID number).
4. If I leave the title incomplete when filing the issue, the core team may close my issue without further explanation or engagement.
5. If I file something completely blank in the body, the core team may close my issue without further explanation or engagement.
6. If I file an issue without collecting logs, the WSL team may close my issue without further explanation or engagement. 

All good? Then proceed!
-->

<!--
This bug tracker is monitored by Windows Subsystem for Linux development team and other technical folks.

Important: When reporting BSODs or security issues, DO NOT attach memory dumps, logs, or traces to Github issues.
Instead, send dumps/traces to secure@microsoft.com, referencing this GitHub issue. Ideally, please configure your machine to capture minidumps, repro the issue, and send the minidump from "C:\Windows\minidump\".
You can find instructions to do that here: https://support.microsoft.com/en-us/help/315263/how-to-read-the-small-memory-dump-file-that-is-created-by-windows-if-a

If this is a console issue (a problem with layout, rendering, colors, etc.), please post the issue to the Terminal tracker: https://github.com/microsoft/terminal/issues
For documentation improvements, please post to the documentation tracker: https://github.com/MicrosoftDocs/WSL/issues
For any other questions on contributing please see our contribution guidelines: https://github.com/Microsoft/WSL/blob/master/CONTRIBUTING.md

Please fill out the items below.
-->

# Environment

```none
Windows build number: [run `cmd.exe /c ver`]
Your Distribution version: [On Debian or Ubuntu run `lsb_release -r` in WSL]
Whether the issue is on WSL 2 and/or WSL 1: [run `cat /proc/version` in WSL]
```

# Steps to reproduce

<!--  What you're doing and what's happening. Copy&paste the full set of specific command-line steps necessary to reproduce the behavior, and their output. Include screenshots if that helps demonstrate the problem. -->

<!-- 
If you'd like to provide logs you can provide an `strace(1)`  log of the failing command (if `some_command` is failing, then run `strace -o some_command.strace -f some_command some_args`, and link the contents of `some_command.strace` in a gist. 
More info on `strace` can be found here: https://www.man7.org/linux/man-pages/man1/strace.1.html
You can use Github gists to share the output: https://gist.github.com/
-->

<!--
Collect WSL logs by following these instructions: https://github.com/Microsoft/WSL/blob/master/CONTRIBUTING.md#8-detailed-logs  
-->
**WSL logs**: 

#  Expected behavior

<!-- A description of what you're expecting, possibly containing screenshots or reference material. -->

# Actual behavior

<!-- What's actually happening? -->



