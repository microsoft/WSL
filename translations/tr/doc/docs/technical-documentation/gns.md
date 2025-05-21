# GNS

`gns`, `mini_init` tarafından oluşturulan bir süreçtir. Görevi WSL2 sanal makinesi içinde ağ yapılandırması yapmaktır.

## Ağ yapılandırması

Ağ ayarları tüm WSL2 dağıtımları tarafından paylaşılır. WSL2 çalışırken, `gns` [wslservice.exe](wslservice.exe.md) için bir hvsocket kanalı tutar ve bu kanal aşağıdaki gibi ağ ile ilgili çeşitli yapılandırmaları göndermek için kullanılır:

- Arayüz IP yapılandırması
- Yönlendirme tablosu girişleri
- DNS yapılandırması
- MTU boyutu yapılandırması

DNS tünelleme etkinleştirildiğinde, `gns` DNS isteklerine yanıt vermekten de sorumludur.

Bkz. `src/linux/init/GnsEngine.cpp` ve `src/windows/service/exe/GnsChannel.cpp`