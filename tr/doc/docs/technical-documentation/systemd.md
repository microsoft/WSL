# Systemd

Bir WSL dağıtımı için Systemd desteği `/etc/wsl.conf` dosyasında aşağıdaki ayarlar yapılarak etkinleştirilebilir:

```
[boot]
systemd=true
```

Etkinleştirildiğinde, [init](init.md) dağıtım başladığında `/sbin/init`i (systemd'nin init'ini işaret eder) başlatır. Bu ayar etkinleştirildiğinde önemli bir fark, [init](init.md)'in verilen dağıtımda pid 1 olmayacağıdır, çünkü systemd'nin init'i pid 1 olarak çalışmayı gerektirir, bu nedenle [init](init.md) fork() olacaktır ve alt süreçte WSL yapılandırmasına devam ederken üstte systemd'yi başlatacaktır. 

`Sbin/init` başlatıldıktan sonra, [init](init.md) systemd'nin hazır olması için `systemctl is-system-running` komutunun `running` ya da `degraded` değerlerinden birini döndürmesini bekler. Belirli bir süre sonunda WSL zaman aşımına uğrayacak ve systemd hazır olmasa bile dağıtımın başlamaya devam etmesine izin verecektir.

## Kullanıcı oturumları

systemd etkinleştirildiğinde, WSL başlatma işlemlerini systemd kullanıcı oturumlarıyla senkronize etmeye çalışır. Bu, şu anda ilişkili systemd kullanıcı oturumunu başlatmak için `login -f <kullanıcı>` başlatılarak yapılmaktadır.

## Ek systemd yapılandırması 

Systemd ile uyumluluğu artırmak için WSL, açılış sırasında (`/run` altında) çeşitli systemd yapılandırma dosyaları oluşturur. Bu konfigürasyon dosyaları şu amaçlarla kullanılır:

- WSL [binfmt interpret](interop.md) dosyasının `systemd-binfmt.service` tarafından silinmesini önleyin
- X11 soketinin `systemd-tmpfiles.serviceè` tarafından silinmesini önleyin 