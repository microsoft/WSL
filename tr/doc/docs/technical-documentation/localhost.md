# Localhost

`localhost` [mini_init](mini_init.md) tarafından oluşturulan bir WSL2 linux işlemidir. Görevi, WSL2 sanal makinesi ile Windows arasında ağ trafiğini iletmektir.


## NAT ağı 

`wsl2.networkingMode` NAT olarak ayarlandığında, `localhost` bağlı TCP bağlantı noktalarını izleyecek ve ağ trafiğini [wslrelay.exe](wslrelay.exe.md) aracılığıyla Windows'a aktaracaktır.

## Yansıtılmış ağ

Yansıtılmış modda, `localhost` `bind()` çağrılarını kesmek için bir BPF programı kaydeder ve çağrıları [wslservice.exe](wslservice.exe.md) aracılığıyla Windows'a iletir, böylece Windows ağ trafiğini doğrudan WSL2 sanal makinesine yönlendirebilir.

Bkz. `src/linux/localhost.cpp`.