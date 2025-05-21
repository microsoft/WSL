# Wslrelay.exe

`wslrelay.exe`, Linux'tan Windows'a ağ ve hata ayıklama konsolu trafiğini aktarmak için kullanılan bir Windows yürütülebilir dosyasıdır. 

Şunlardan sorumludur:

- NAT modunda Linux ve Windows arasında localhost trafiğini aktarma (bkz. [localhost](localhost.md))
- `wsl2.debugConsole` öğesi `true` olarak ayarlandığında hata ayıklama konsolu çıktısının görüntülenmesi

Bkz. `src/windows/wslrelay/main.cpp`