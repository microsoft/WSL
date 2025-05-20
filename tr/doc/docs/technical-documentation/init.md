# Init

`init`, bir WSL dağıtımının en üst düzey (ana) işlemidir. WSL1 dağıtımları için, `init` [wslservice](wslservice.exe.md) tarafından başlatılır (bkz: `src/windows/service/LxssInstance.cpp`), WSL2 dağıtımları için ise [mini_init](mini_init.md) tarafından başlatılır.

## WSL2’ye Özgü Dağıtım Başlatma Süreci

Her WSL2 dağıtımı, ayrı bir bağlama (mount), PID ve UTS isim alanı (namespace) içinde çalışır. Bu sayede dağıtımlar birbirlerinden izole şekilde paralel olarak çalışabilir.

Bir WSL2 dağıtımı başlatıldığında, [mini_init](mini_init.md) şu işlemleri yapar:

- Dağıtıma ait VHD dosyasını bağlar (mount eder)
- Yeni bir alt isim alanına (namespace) çatallanır (clone)
- Bu yeni alanda VHD bağlama noktasına chroot yapar
- `init` işlemini çalıştırır (Bkz: `LxMiniInitMessageLaunchInit` mesajı)

Her dağıtım kendi mount namespace’inde çalışmasına rağmen, `/mnt/wsl` bağlama noktası tüm dağıtımlar arasında ortaktır.

## Dağıtım Başlatma (İlklendirme)

`init` işlemi başlatıldığında, aşağıdaki ilklendirme görevlerini yerine getirir:

- `/proc`, `/sys` ve `/dev` dizinlerini bağlar
- `cgroups` yapılandırmasını gerçekleştirir
- `binfmt` yorumlayıcısını kaydeder (Bkz: [interop](interop.md))
- [/etc/wsl.conf](https://learn.microsoft.com/windows/wsl/wsl-config) dosyasını okur
- `systemd` işlemini başlatır (Bkz: [systemd](systemd.md))
- `drvfs` sürücülerini bağlar (Bkz: [drvfs](drvfs.md))
- `wslg` yapılandırmasını gerçekleştirir (Bkz: [wslg](https://github.com/microsoft/wslg))

## Dağıtımı Çalıştırma

Tüm işlemler tamamlandığında, `init` işlemi [wslservice](wslservice.exe.md) ile bir bağlantı kurar: WSL1 için `lxbus`, WSL2 içinse `hvsocket`. Bu kanal aracılığıyla `init` işlemine çeşitli komutlar iletilir (Bkz: `src/shared/inc/lxinitshared.h`). Bu komutlardan bazıları şunlardır:

- `LxInitMessageInitialize`: Dağıtımı yapılandır
- `LxInitMessageCreateSession`: Yeni bir oturum lideri oluştur. (Bkz: [session leader](session-leader.md))
- `LxInitMessageTerminateInstance`: Dağıtımı sonlandır
