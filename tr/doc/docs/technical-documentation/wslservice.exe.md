# wslservice.exe

WslService, SYSTEM olarak çalışan bir oturum 0 hizmetidir. Görevi WSL oturumlarını yönetmek, WSL2 sanal makinesi ile iletişim kurmak ve WSL dağıtımlarını yapılandırmaktır. 

## COM Arayüzü

İstemciler WslService'e COM arayüzü olan ILxssUserSession aracılığıyla bağlanabilir. Bu arayüzün tanımı `src/windows/service/incwslservice.idl` dosyasında bulunabilir.

Bir COM istemcisi bu arayüz üzerinde [CoCreateInstance()](https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance) çağrısı yaptığında, hizmet `LxssUserSessionFactory` (bkz. `src/windows/service/LxssUserSessionFactory.cpp`) aracılığıyla istekleri alır ve her Windows kullanıcısı için bir `LxssUserSession` (bkz. `src/windows/service/LxssUserSession.cpp`) örneği döndürür (aynı Windows kullanıcı hesaplarından birden çok kez CoCreateInstance() çağrısı yapılması aynı örneği döndürür).

İstemci daha sonra `ILxssUserSession` örneğini kullanarak hizmete aşağıdaki gibi yöntemler çağırabilir:

- `CreateInstance()`: Bir WSL dağıtımı başlatma
- `CreateLxProcess()`: Bir dağıtım içinde bir süreç başlatın
- `RegisterDistribution()`: Yeni bir WSL dağıtımı kaydedin
- `Shutdown()`: Tüm WSL dağıtımlarını sonlandırın

## WSL2 Sanal makine

WslService WSL2 Sanal Makinesini yönetir. Sanal makine yönetim mantığı `src/windows/service/WslCoreVm.cpp` dosyasında bulunabilir. 

Önyüklendikten sonra WslService, Linux süreçlerine çeşitli komutlar göndermek için kullandığı Sanal Makine ile bir [hvsocket](https://learn.microsoft.com/virtualization/hyper-v-on-windows/user-guide/make-integration-service) tutar (daha fazla ayrıntı için [mini_init](mini_init.md) bölümüne bakın). 


## WSL2 Dağıtımları 

Sanal makine çalıştıktan sonra, WSL dağıtımları `WslCoreVm::CreateInstance` çağrılarak başlatılabilir. Çalışan her dağıtım bir `WslCoreInstance` ile temsil edilir (bkz. `src/windows/service/WslCoreInstance.cpp`).

Her `WslCoreInstance` [init](init.md) ile bir hvsocket bağlantısı kurarak WslService'in aşağıdaki gibi çeşitli görevleri yerine getirmesini sağlar:

- Dağıtım içinde süreçleri başlatma
- Dağıtımdan çıkıldığında haberdar olun
- drvfs paylaşımlarını bağlama (/mnt/*)
- Dağıtımı durdurun