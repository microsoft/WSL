# WSL Overview

WSL is comprised of a set of executables, API's and protocols. This page offers an overview of the different components, and how they're connected.
Click on any component to get more details.


```mermaid
%%{ init: {
    'flowchart': { 'curve': 'stepBefore' },
    'theme': 'neutral'
    }
}%%
graph
  subgraph Windows["<b><p style="font-size:30px">Windows</p></b>"]
      C:\Windows\System32\wsl.exe["C:\Windows\System32\wsl.exe"]---|"CreateProcess()"|wsl.exe;
      wsl.exe[<a href="wsl.exe">wsl.exe</a>]---|COM|wslservice.exe;
      wslg.exe[<a href="wslg.exe">wslg.exe</a>]---|COM|wslservice.exe;
      wslconfig.exe[<a href="wslconfig.exe">wslconfig.exe</a>]---|COM|wslservice.exe;
      wslapi.dll[<a href="https://learn.microsoft.com/windows/win32/api/wslapi/">wslapi.dll</a>]---|COM|wslservice.exe;
      id[debian.exe, ubuntu.exe, ]---|"LoadLibrary()"|wslapi.dll;
      wslservice.exe[<a href="wslservice.exe">wslservice.exe</a>]---|"CreateProcessAsUser()"|wslrelay.exe[<a href="wslrelay.exe">wslrelay.exe</a>];
      wslservice.exe---|"CreateProcessAsUser()"|wslhost.exe[<a href="wslhost.exe">wslhost.exe</a>];
      fs["Windows filesystem (//wsl.localhost)"]
  end
  
  wslservice.exe -----|hvsocket| mini_init
  wslservice.exe -----|hvsocket| gns
  fs---|hvsocket|plan9

  wsl.exe---|hvsocket|relay
  
  subgraph Linux["<b><p style="font-size:30px">Linux</p></b>"]
      mini_init[<a href="mini_init">mini_init</a>]---|"exec()"|gns[<a href="gns">gns</a>]
      mini_init---|"exec()"|init[<a href="init">init</a>];
      mini_init---|"exec()"|localhost[<a href="localhost">localhost</a>];
      
      subgraph "Linux Distribution"["<b><p style="font-size:23px">Linux Distribution</p></b>"]

          init[<a href="init">init</a>]---|"exec()"|plan9[<a href="plan9">plan9</a>];
          init---|"exec()"|sid[session leader];
          sid[<a href="session-leader">session leader</a>]---|"exec()"|relay
          relay[<a href="relay">relay</a>]---|"exec()"|cid["User command (bash, curl)"]
      end

  end
```