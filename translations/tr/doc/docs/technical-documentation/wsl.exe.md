# wsl.exe

wsl.exe WSL'nin ana komut satırı giriş noktasıdır. Görevi şudur:

- Komut satırı argümanlarını ayrıştırma (Bkz. `src/windows/common/wslclient.cpp`)
- WSL'yi başlatmak için COM aracılığıyla [wslservice.exe](wslservice.exe.md) çağrısı yapın (bkz. `src/windows/common/svccomm.cpp`)
- Linux sürecinden ve linux sürecine stdin / stdout / stderr aktarımı