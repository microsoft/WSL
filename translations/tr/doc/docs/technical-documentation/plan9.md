# Plan 9

Plan9, WSL1 ve WSL2 dağıtımları için bir plan9 dosya sistemi sunucusu barındıran bir linux işlemidir. Her dağıtımda [init](init.md) tarafından oluşturulur.

## WSL 1 

WSL1 dağıtımlarında, `plan9` dosya sistemini bir unix soketi üzerinden sunar ve bu sokete daha sonra Windows'tan bağlanılabilir.

## WSL2 

WSL2 dağıtımlarında, `plan9` dosya sistemini bir `hvsocket` üzerinden çalıştırır

## Dağıtım Dosyalarına Windows'tan Erişme

Windows'tan, özel bir yönlendirici sürücü (p9rdr.sys) hem `\\wsl$` hem de `\\wsl.localhost` adreslerini kaydeder. Bu yollardan herhangi birine erişildiğinde, `p9rdr.sys` belirli bir Windows kullanıcısı için mevcut dağıtımları listelemek üzere [wslservice.exe] (wslservice.exe.md) dosyasını çağırır.

Bir dağıtım yoluna erişildiğinde (`\\wsl.localhost\debian` gibi), `p9rdr.sys` dağıtımı başlatmak için COM aracılığıyla [wslservice.exe](wslservice.exe.md) dosyasını çağırır ve dosyalara Windows'tan erişilmesini sağlayan plan9 sunucusuna bağlanır. 

Bkz. `src/linux/init/plan9.cpp`