# WSL Genel Bakış

WSL, çeşitli yürütülebilir dosyalardan, API'lerden ve protokollerden oluşur. Bu sayfa, bileşenlerin genel yapısını ve birbirleriyle nasıl bağlantılı olduklarını özetler.

Her bir bileşen hakkında daha fazla bilgi almak için üzerine tıklayabilirsiniz.

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
      fs["Windows dosya sistemi (//wsl.localhost)"]
  end
  
  wslservice.exe -----|hvsocket| mini_init
  wslservice.exe -----|hvsocket| gns
  fs---|hvsocket|plan9

  wsl.exe---|hvsocket|relay
  
  subgraph Linux["<b><p style="font-size:30px">Linux</p></b>"]
      mini_init[<a href="mini_init">mini_init</a>]---|"exec()"|gns[<a href="gns">gns</a>]
      mini_init---|"exec()"|init[<a href="init">init</a>];
      mini_init---|"exec()"|localhost[<a href="localhost">localhost</a>];
      
      subgraph "Linux Dağıtımı"["<b><p style="font-size:23px">Linux Dağıtımı</p></b>"]

          init[<a href="init">init</a>]---|"exec()"|plan9[<a href="plan9">plan9</a>];
          init---|"exec()"|sid[session leader];
          sid[<a href="session-leader">session leader</a>]---|"exec()"|relay
          relay[<a href="relay">relay</a>]---|"exec()"|cid["Kullanıcı komutu
          (bash, curl)"]
      end

  end
```