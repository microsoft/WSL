# Building WSL

## Önkoşullar  

WSL'yi derlemek için aşağıdaki araçlar gereklidir: 

- CMake >= 2.25
    - `winget install Kitware.CMake` ile kurulabilir.
- Aşağıdaki bileşenlere sahip Visual Studio:
    - Windows SDK 26100
    - MSBuild
    - v143 derleme araçları için evrensel Windows platformu desteği (X64 ve ARM64)
    - MSVC v143 - VS 2020 C++ ARM64 derleme araçları (En Son + Spectre) (X64 ve ARM64)
    - C++ temel özellikleri
    - En son v143 araçları için C++ ATL (X64 ve ARM64)
    - Windows için C++ Clang derleyicisi
    - .NET masaüstü geliştirme
    - .NET WinUI uygulama geliştirme araçları
    
## Building WSL

Depoyu klonladıktan sonra, çalıştırarak Visual Studio çözümünü oluşturun:

```
cmake .
```

Bu, Visual Studio ile veya `cmake --build .` aracılığıyla oluşturabileceğiniz bir `wsl.sln` dosyası oluşturacaktır.

Derleme parametreleri:

- `cmake . -A arm64`: ARM64 için bir paket oluştur
- `cmake . -DCMAKE_BUILD_TYPE=Release`: Sürüm için derle
- `cmake . -DBUILD_BUNDLE=TRUE`: Bir paket msix paketi oluştur (önce ARM64 oluşturmayı gerektirir)

Not: Geliştirme sırasında daha hızlı derleme ve dağıtım yapmak için `UserConfig.cmake` dosyasındaki seçeneklere bakın.

## WSL'yi Dağıtma  

Derleme tamamlandıktan sonra, WSL'yi `bin\<platform>\<hedef>\wsl.msi` altında bulunan MSI paketini yükleyerek veya `powershell tools\deploy\deploy-to-host.ps1` dosyasını çalıştırarak kurabilirsiniz.

Bir Hyper-V sanal makinesine dağıtmak için `powershell tools\deploy\deploy-to-vm.ps1 -VmName <vm> -Username <kullanıcı adı> -Password <parola>` kullanabilirsiniz

## Çalışan testler

Birim testlerini çalıştırmak için çalıştırın: `bin\<platform>\<hedef>\test.bat`. Oldukça fazla test var, bu yüzden muhtemelen her şeyi çalıştırmak istemezsiniz. İşte makul bir alt küme:
`bin\<platform>\<hedef>\test.bat /name:*UnitTest*`

Belirli bir test senaryosunu çalıştırmak için çalıştırın:
`bin\<platform>\<hedef>\test.bat /name:<class>::<test>`
Örnek: `bin\x64\debug\test.bat /name:UnitTests::UnitTests::ModernInstall` 

WSL1 için testleri çalıştırmak için `-Version 1` ekleyin. 
Örnek: `bin\x64\debug\test.bat -Version 1` 


Testleri bir kez çalıştırdıktan sonra, paket kurulumunu atlamak için `-f` ekleyebilirsiniz, bu da testleri daha hızlı hale getirir (bu, test_distro'nun varsayılan WSL dağıtımı olmasını gerektirir).

Örnek:

```
wsl --set-default test_distro
bin\x64\debug\test.bat /name:*UnitTest* -f
```

## Hata ayıklama testleri

Genel hata ayıklama talimatları için [debugging] (debugging.md) bölümüne bakın.

Birim test sürecine bir hata ayıklama eklemek için şunu kullanın: `test.bat` dosyasını çağırırken  `/waitfordebugger` kullanın. 
İlk test hatasında otomatik olarak kesmek için `/breakonfailure` kullanın.