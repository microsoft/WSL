# Plan 9

Plan9 is a linux process that hosts a plan9 filesystem server for WSL1 and WSL2 distributions. It's created by [init](init.md) in each distribution.

## WSL 1 

In WSL1 distributions, `plan9` serves its filesystem through a unix socket, which can then be connected to from Windows.

## WSL2 

In WSL2 distributions, `plan9` runs its filesystem through an `hvsocket`

## Accessing the distribution files from Windows

From Windows, a special redirector driver (p9rdr.sys) registers both `\\wsl$` and `\\wsl.localhost`. When either of those paths are accessed, `p9rdr.sys` calls [wslservice.exe](wslservice.exe.md) to list the available distributions for a given Windows user.

When a distribution path is accessed (like `\\wsl.localhost\debian`), `p9rdr.sys` calls into [wslservice.exe](wslservice.exe.md) via COM to start the distribution, and connect to its plan9 server, which allows the files to be accessed from Windows. 

See `src/linux/init/plan9.cpp`