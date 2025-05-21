# Wslhost.exe 

`wslhost.exe` masaüstü bildirimlerini görüntülemek ve arka planda Linux işlemlerini çalıştırmak için kullanılan bir Windows yürütülebilir dosyasıdır.

## COM Sunucusu

COM sunucusu olarak çalışırken, `wslhost.exe` bir [NotificationActivatorFactory](https://learn.microsoft.com/dotnet/api/microsoft.toolkit.uwp.notifications.notificationactivator?view=win-comm-toolkit-dotnet-7.1) kaydeder ve bu daha sonra kullanıcıya masaüstü bildirimlerini görüntülemek için kullanılır.

Bildirimler şu amaçlarla kullanılabilir:

- Kullanıcıyı WSL güncellemesi hakkında bilgilendirin
- Kullanıcıyı bir yapılandırma hatası hakkında uyarın
- Kullanıcıyı proxy değişikliği hakkında bilgilendirme

Bakınız: `src/windows/common/notifications.cpp`

## Arka Plan İşlemleri 

[wsl.exe](wsl.exe.md) ilişkili Linux süreçleri sonlanmadan önce sonlandığında, `wslhost.exe` Linux sürecinin yaşam süresini devralır. 

Bu, Linux işlemlerinin Windows komutlarını çalıştırmaya devam etmesini ve ilişkili `wsl.exe` sonlandırıldıktan sonra bile terminale erişmesini sağlar. 

Bkz. `src/windows/wslhost/main.cpp` ve [interop](interop.md)