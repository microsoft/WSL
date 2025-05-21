# WSL'de Hata Ayıklama

## Log Kaydı

WSL'de birden fazla log kaydı kaynağı vardır. Bunlardan en önemlisi Windows işlemlerinden yayılan ETL izidir.

Bir ETL izi toplamak için ([wsl.wprp bağlantısı](https://github.com/microsoft/WSL/blob/master/diagnostics/wsl.wprp)) çalıştırın:

```
wpr -start wsl.wprp -filemode

[sorunu yeniden oluşturun]

wpr -stop logs.ETL
```

Log dosyası kaydedildikten sonra, logları görüntülemek için [WPA](https://apps.microsoft.com/detail/9n58qrw40dfw?hl=en-US&gl=US) adresini kullanabilirsiniz.

Önemli ETL sağlayıcıları: 

- `Microsoft.Windows.Lxss.Manager`: wslservice.exe'den yayılan loglar.
    Önemli olaylar: 
    - `GuestLog`: Vm'nin dmesg'sinden loglar
    - `Error`: Beklenmeyen hatalar
    - `CreateVmBegin`, `CreateVmEnd`: Sanal makine yaşam süresi
    - `CreateNetworkBegin`, `CreateNetworkEnd`: Ağ yapılandırması
    - `SentMessage`, `ReceivedMessaged`: Linux ile hvsocket kanalları üzerinde iletişim.
    
- `Microsoft.Windows.Subsystem.Lxss`: Diğer WSL yürütülebilir dosyaları (wsl.exe, wslg.exe, wslconfig.exe, wslrelay.exe, ...)
    Önemli olaylar:
    - `UserVisibleError`: Kullanıcıya bir hata gösterildi 

- `Microsoft.Windows.Plan9.Server`: Windows plan9 sunucusundan gelen loglar (/mnt/ paylaşımlarına erişirken ve Windows'u çalıştırırken kullanılır)


Linux tarafında, loglara erişmenin en kolay yolu `dmesg`ye bakmak veya yazarak etkinleştirilebilen hata ayıklama konsolunu kullanmaktır:

```
[wsl2]
debugConsole=true
```

dosyasını `%USERPROFILE%/.wslconfig` olarak değiştirin ve WSL'yi yeniden başlatın


## Hata Ayıklayıcıları Ekleme

Usermode WSL Windows işlemlerine eklenebilir (wsl.exe, wslservice.exe, wslrelay.exe, ...). Semboller `bin/<platform>/<hedef>` klasörü altında mevcuttur. 

İşlemler çöktüğünde otomatik olarak çökme dökümlerini toplamak için [bu numarayı](https://github.com/microsoft/WSL/blob/master/CONTRIBUTING.md#11-reporting-a-wsl-process-crash) da kullanabilirsiniz.

## Linux Hata Ayıklama

`gdb` Linux süreçlerine eklenebilir (bkz. [man gdb](https://man7.org/linux/man-pages/man1/gdb.1.html)). 

Bir WSL işleminde gdb ile hata ayıklamanın en basit yolu, koda gdb'den erişmek için `/mnt` bağlama noktalarını kullanmaktır. 
Başladıktan sonra, kaynak dosyalara bağlanmak için gdb'de `dir /path/to/wsl/source` kullanın.

## Root Namespace Hata Ayıklama

`gns` veya `mini_init` gibi bazı WSL işlemlerine WSL dağıtımları içinden erişilemez. Bunlara bir hata ayıklayıcı eklemek için, hata ayıklama kabuğunu kullanın:

```
wsl --debug-shell
```

Daha sonra `tdnf install gdb` komutunu çalıştırarak `gdb`yi kurabilir ve hata ayıklama işlemlerini başlatabilirsiniz.