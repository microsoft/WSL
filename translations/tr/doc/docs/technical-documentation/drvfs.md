# Linux'tan Windows Sürücülerine Erişim

WSL, Linux'tan Windows sürücülerine erişim için mount noktaları sunar. Bu mount noktaları varsayılan olarak `/mnt` altında bulunur ve Windows sürücülerinin kök dizinlerini işaret eder.

## Yetkili (elevated) ve Yetkisiz (non-elevated) Mount Noktaları

Bir dağıtım içinde, WSL Linux süreçlerini yönetici yetkisiyle (elevated) mi yoksa kullanıcı yetkisiyle (non-elevated) mi başlatıldıklarına göre ayırır.

Bu ayrım, dağıtım içinde iki ayrı [mount namespace](https://man7.org/linux/man-pages/man7/mount_namespaces.7.html) kullanılarak yapılır. Bu namespace'lerden biri Windows sürücülerine yetkili erişim sağlar, diğeri ise yetkisiz erişim sağlar.

Yeni bir Linux süreci oluşturulduğunda, [wslservice.exe](wslservice.exe.md) sürecin yetki durumunu belirler ve ardından [init](init.md) işlemine süreci uygun mount namespace içinde başlatmasını söyler.

## Bir Windows Sürücüsünü Mount Etme

> **Not:** Bu bölüm yalnızca WSL2 dağıtımları için geçerlidir.

Bir [oturum lideri (session leader)](session-leader.md) oluşturulduğunda, [wslservice.exe](wslservice.exe.md) bir [plan9](https://9fans.github.io/plan9port/man/man9/intro.html) dosya sunucusu başlatır. Bu dosya sunucusuna WSL2 sanal makinesinden bağlanarak Windows sürücüleri mount edilebilir.

WSL dağıtımı oluşturulurken, [wslservice.exe](wslservice.exe.md) `LX_INIT_CONFIGURATION_INFORMATION` mesajını kullanarak dağıtımı başlatan sürecin yetkili olup olmadığını belirtir. Buna göre, [init](init.md) plan9 sunucusunun ya yetkili ya da yetkisiz sürümünü mount eder.

Daha sonra, henüz mount edilmemiş diğer namespace’te (ya yetkili ya da yetkisiz) ilk komut oluşturulduğunda, [wslservice.exe](wslservice.exe.md), [init](init.md) işlemine `LxInitMessageRemountDrvfs` mesajını gönderir. Bu mesaj, `init`'e diğer namespace’i de mount etmesini söyler.

Daha fazla bilgi için: `src/windows/service/exe/WslCoreInstance.cpp` ve `src/linux/drvfs.cpp`.

## Linux Üzerinden Bir Sürücü Mount Etme

Windows plan9 sunucusu çalıştığı sürece, sürücüler doğrudan [mount](https://linux.die.net/man/8/mount) komutu kullanılarak Linux'tan mount edilebilir. Örneğin, C: sürücüsünü manuel olarak mount etmek için şu komut kullanılabilir:

```
mount -t drvfs C: /tmp/my-mount-point
```

Bu işlem dahili olarak `/usr/sbin/mount.drvfs` tarafından gerçekleştirilir; bu dosya aslında `/init`'e sembolik bir bağlantıdır. `/init` başlatıldığında, hangi giriş noktasının çalıştırılacağını belirlemek için `argv[0]` değerine bakar. Eğer `argv[0]` değeri `mount.drvfs` ise, `/init` `mount.drvfs` giriş noktasını çalıştırır (Bkz: `MountDrvfsEntry()` `src/linux/init/drvfs.cpp` içinde).

Dağıtımın yapılandırmasına bağlı olarak, `mount.drvfs` sürücüyü `drvfs` (WSL1) olarak ya da `plan9`, `virtio-plan9` veya `virtiofs` (WSL2) olarak mount eder. Bu davranış [.wslconfig](https://learn.microsoft.com/windows/wsl/wsl-config) dosyasına göre belirlenir.