# Linux'tan Windows Yürütülebilir Dosyalarını Çalıştırmak

Linux'tan Windows işlemleri başlatma yeteneği iki farklı ayar düzeyi tarafından kontrol edilir:

- Tüm Windows kullanıcıları için geçerli olan `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\LxssManager\DistributionFlags` kayıt defteri değeri (en düşük anlamlı bit ayarlanırsa interop devre dışı kalır)
- Belirli bir WSL dağıtımı için geçerli olan [/etc/wsl.conf](https://learn.microsoft.com/windows/wsl/wsl-config#wslconf) dosyasındaki `[interop]` bölümü

## Windows Yürütülebilir Dosyaları için binfmt Yorumlayıcıları

Linux üzerinden Windows işlemlerinin oluşturulabilmesi için, WSL bir [binfmt yorumlayıcısı](https://docs.kernel.org/admin-guide/binfmt-misc.html) kaydeder. Bu yorumlayıcı, belirli bir yürütülebilir dosya türü `exec*()` sistem çağrıları ile başlatıldığında çekirdeğe hangi komutun çalıştırılacağını söyler.

Bu kaydı gerçekleştirmek için, WSL `/proc/sys/fs/binfmt_misc` dizinine yazar ve `/init` dosyasına işaret eden bir `WSLInterop` girdisi oluşturur. WSL1 için bu kayıt, her dağıtım için [init](init.md) tarafından yapılır; WSL2 içinse kayıt işlemi sanal makine düzeyinde [mini_init](mini_init.md) tarafından gerçekleştirilir.

Not: `/init` yürütülebilir dosyası, farklı WSL süreçleri ([init](init.md), [plan9](plan9.md), [localhost](localhost.md), vb.) için giriş noktasıdır. Bu yürütülebilir dosya, hangi işlemi çalıştıracağını `argv[0]` parametresine bakarak belirler. Eğer `argv[0]` bilinen giriş noktalarından biriyle eşleşmiyorsa, interop bağlamında `/init` Windows işlemi başlatma mantığını çalıştırır.

Bkz: `WslEntryPoint()` fonksiyonu — `src/linux/init.cpp` dosyasında.

## Interop Sunucularına Bağlanma

Kullanıcı bir Windows işlemi başlatmaya çalıştığında, çekirdek `/init` dosyasını ilgili Windows komut satırı argümanlarıyla birlikte çalıştırır.

Yeni bir Windows işlemi başlatmak için `/init` bir interop sunucusuna bağlanmak zorundadır. Interop sunucuları, Windows işlemleriyle hvsocket bağlantısı kurabilen Linux süreçleridir (genellikle [wsl.exe](wsl.exe.md) veya [wslhost.exe](wslhost.exe.md)) ve Windows yürütülebilir dosyalarını başlatabilirler.

Linux içinde, her bir [oturum lideri](session-leader.md) ve her bir [init](init.md) örneği ile ilişkili bir interop sunucusu bulunur. Bu sunucu, `/run/WSL` altındaki bir Unix soketi üzerinden hizmet verir.

`/init`, hangi sunucuya bağlanacağını belirlemek için `$WSL_INTEROP` ortam değişkenini kullanır. Eğer bu değişken ayarlanmamışsa, `/init` kendi PID'si ile `/run/WSL/${pid}_interop` yoluna bağlanmayı dener. Bu başarısız olursa, ebeveyn sürecinin PID'sini dener ve bu şekilde [init](init.md)'e kadar yukarı doğru devam eder.

Bağlantı kurulduktan sonra `/init`, bir `LxInitMessageCreateProcess` (WSL1) ya da `LxInitMessageCreateProcessUtilityVm` (WSL2) mesajı gönderir. Bu mesaj, ilgili Windows sürecine iletilir ve istenilen komut başlatılarak çıktısı `/init`'e aktarılır.

Bkz: `src/linux/init/binfmt.cpp`
