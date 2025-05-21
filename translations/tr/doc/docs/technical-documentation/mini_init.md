# mini_init

mini_init, WSL2 sanal makinesi başlatıldığında çalışan ilk yürütülebilir dosyadır. Daha fazla detay için [WSL2 boot process](boot-process.md) bölümüne bakabilirsiniz.

## Sanal Makine Kurulumu

`mini_init`, çekirdek önyüklemeyi tamamladığında başlatılır ve `/init` dosyasını çağırır, bu da aslında `mini_init`'tir. Diğer standart Linux `init` çalıştırılabilir dosyaları gibi `mini_init` de başlangıçta `/proc`, `/sys`, `/dev` ve diğer standart bağlama noktalarını bağlar.

Daha sonra `mini_init`, çökme dökümü (crash dump) toplama özelliğini etkinleştirme, `/dev/console` üzerinden günlükleme yapılandırması ve tty yapılandırması gibi çeşitli ayarları yapar.

Her şey hazır olduğunda, `mini_init`, [wslservice](wslservice.exe.md) ile iki hvsocket bağlantısı kurar.

Bu bağlantılardan biri "mini_init" kanalı olarak adlandırılır ve `wslservice.exe` tarafından gönderilen mesajlar için kullanılır. Mesajlar ve yanıtların listesi için `src/shared/inc/lxinitshared.h` dosyasına bakabilirsiniz. Yaygın mesajlar şunlardır:

- `LxMiniInitMessageLaunchInit`: Sanal bir diski bağlar ve yeni bir dağıtım başlatır. Ayrıntılar için [init](init.md) bölümüne bakın.
- `LxMiniInitMessageMount`: `/mnt/wsl` konumuna bir disk bağlar (wsl --mount komutu için kullanılır)
- `EJECT_VHD_MESSAGE`: Bir diski çıkarır
- `LxMiniInitMessageImport`: Bir dağıtımı içe aktarır
- `LxMiniInitMessageExport`: Bir dağıtımı dışa aktarır

Diğer hvsocket kanalı ise [wslservice.exe](wslservice.exe.md) dosyasına bildirim göndermek için kullanılır. Bu genellikle Linux işlemleri sona erdiğinde rapor vermek için kullanılır (wslservice, dağıtımların ne zaman sonlandığını bu şekilde öğrenir).

## Ağ Yapılandırması

Önyükleme sürecinin bir parçası olarak, `mini_init` ayrıca ağ yapılandırmasını yöneten [gns binary](gns.md) dosyasını da başlatır.

## Diğer Görevler

`mini_init` ayrıca aşağıdaki gibi çeşitli bakım görevlerini yerine getirir:

- Kullanılmayan belleği geri kazanma
- Hata ayıklama kabuğu tty'sini başlatma
- Sanal makine sonlandırıldığında G/Ç (IO) senkronizasyonu yapma
- Dosya sistemini yeniden boyutlandırma (örneğin `wsl --manage <distro> --resize`)
- Disk biçimlendirme (yeni dağıtımlar yüklenirken kullanılır)