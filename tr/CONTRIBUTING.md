# WSL Katkı Rehberi

WSL'ye katkıda bulunmanın birkaç ana yolu vardır. Her biri için kılavuzlar aşağıdadır:

1. [WSL'ye bir özellik ekleme veya hata düzeltme](#add-a-feature-or-bugfix-to-wsl)
2. [WSL için bir sorun bildirme](#file-a-wsl-issue)

## WSL'ye Bir Özellik Ekleme veya Hata Düzeltme

WSL kaynak koduna özellik eklemek veya hataları düzeltmek için yapacağınız her türlü katkıyı memnuniyetle karşılıyoruz! Çalışmaya başlamadan önce, lütfen **[bunu bu depoda bir sorun veya özellik isteği olarak bildirin](https://github.com/microsoft/WSL/issues)**, böylece süreci takip edebilir ve gerekirse geri bildirim sağlayabiliriz.

Bu adımı tamamladıktan sonra, geliştirme amacıyla WSL’yi kendi bilgisayarınızda nasıl derleyeceğinizi öğrenmek için [geliştirici dokümantasyonunu](./doc/docs/dev-loop.md) inceleyin.

Düzenlemeleriniz hazır olduğunda, lütfen [bu depoda bir pull request (çekme isteği) olarak gönderin](https://github.com/microsoft/WSL/pulls). WSL ekibi talebi değerlendirecek ve geri dönüş yapacaktır. Çoğu katkı, bir Katılımcı Lisans Sözleşmesi (CLA) imzalamanızı gerektirir. Bu sözleşme, bize katkınızı kullanma hakkı verdiğinizi beyan eder. Detaylar için: https://cla.microsoft.com.


## WSL İçin Sorun Bildirme

WSL ile ilgili sorunları bu depoya ya da bağlı diğer depolara bildirebilirsiniz. Yeni bir sorun oluşturmadan önce benzer sorunlar var mı diye arama yapın, varsa onları oylayın veya yorum yapın.

1. Sorununuz WSL dokümantasyonu ile ilgiliyse, lütfen [microsoftdocs/wsl](https://github.com/microsoftdocs/WSL/issues) deposuna bildirin.
2. Sorununuz bir Linux grafik arayüz uygulamasıyla ilgiliyse, lütfen [microsoft/wslg](https://github.com/microsoft/wslg/issues) deposuna bildirin.
3. Diğer tüm teknik sorunlar (örneğin WSL başlatma problemleri) için lütfen [microsoft/wsl](https://github.com/microsoft/WSL/issues) deposuna bildirin.


Lütfen hata bildirirken mümkün olduğunca fazla bilgi sağlayın ve gerekirse log dosyalarını da ekleyin.

[WSL logları toplama notları](#notes-for-collecting-wsl-logs) bölümüne göz atmayı unutmayın.

## Teşekkürler

Katkılarınız için şimdiden teşekkür ederiz! WSL’yi herkes için daha iyi bir araç yapmak için desteğinizi takdir ediyoruz.

## WSL Logları Toplama Notları

### Önemli: BSOD (Mavi Ekran) ve Güvenlik Sorunlarını Bildirme
**Windows çökmesi (BSOD) veya güvenlik sorunları için GitHub’da sorun oluşturmayın.**. Bunun yerine, bu tür durumları secure@microsoft.com adresine e-posta ile bildirin. Detaylı bilgi için aşağıdaki `10) Reporting a Windows crash (BSOD)` bölümüne bakın.

### WSL Arayüz / Metin Görselleştirme / Kullanıcı Deneyimi Sorunları
WSL dağıtımları genellikle Windows Konsolu’nda açılır (farklı bir terminal ayarlamadıysanız). Bu nedenle, konsol arayüzü ile ilgili sorunlar için lütfen [Windows Console hata izleyiciye](https://github.com/microsoft/console) bildirimde bulunun.

### Ağ Sorunları İçin WSL Loglarını Toplama

Aşağıdaki komutlarla WSL dağıtımınızda tcpdump kurun (İnternet bağlantısı olmalı):

```
# sudo apt-get update
# sudo apt-get -y install tcpdump
```

Ayrıca [WPR](https://learn.microsoft.com/windows-hardware/test/wpt/windows-performance-recorder) kurmalısınız.

Logları toplamak için Yönetici PowerShell penceresinde şu komutları çalıştırın:

```
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/collect-networking-logs.ps1" -OutFile collect-networking-logs.ps1
Set-ExecutionPolicy Bypass -Scope Process -Force
.\collect-networking-logs.ps1
```
Komutlar çalıştıktan sonra problemi yeniden oluşturun ve ardından bir tuşa basarak log toplama işlemini sonlandırın. Dosya konumu ekrana yazılacaktır.

<!-- Preserving anchors -->
<div id="8-detailed-logs"></div>
<div id="9-networking-logs"></div>
<div id="8-collect-wsl-logs-recommended-method"></div>


### WSL Loglarını Toplama (Tavsiye Edilen Yöntem)

Logları GitHub'a yüklemek yerine e-posta ile göndermek isterseniz, wsl-gh-logs@microsoft.com adresine gönderin. E-posta başlığına ilgili GitHub sorun numarasını eklemeyi ve mesajda yorum bağlantısını paylaşmayı unutmayın.

Komutlar:

```
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/collect-wsl-logs.ps1" -OutFile collect-wsl-logs.ps1
Set-ExecutionPolicy Bypass -Scope Process -Force
.\collect-wsl-logs.ps1
```
Kod tamamlandığında günlük dosyasının yolunu çıktılayacaktır.

### 10) Windows Çökmesini (BSOD) Bildirme

Kernel dump dosyası toplamak için yönetici komut satırında:

```
reg.exe add HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\CrashControl /v AlwaysKeepMemoryDump  /t REG_DWORD /d 1 /f
```

Sorunu yeniden oluşturun ve sistemin çökmesine izin verin. Çökmeden sonra C:\Windows\MEMORY.DMP dosyasını bulun. Bu dosyayı secure@microsoft.com adresine gönderin. E-posta içinde:

- GitHub sorun numarası (varsa)
- WSL ekibine gönderildiği bilgisi

olmalıdır.

### 11) WSL Süreç Çökmesini Bildirme

Bir WSL süreci çökmesini bildirmenin en kolay yolu [kullanıcı modu çökme dökümü toplamaktır](https://learn.microsoft.com/windows/win32/wer/collecting-user-mode-dumps).

Çalışan tüm WSL işlemlerinin dökümlerini toplamak için, lütfen yönetici ayrıcalıklarına sahip bir PowerShell istemi açın, günlük dosyalarınızı koymak istediğiniz bir klasöre gidin ve aşağıdaki komutları çalıştırın: 

```
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/collect-wsl-logs.ps1" -OutFile collect-wsl-logs.ps1
Set-ExecutionPolicy Bypass -Scope Process -Force
.\collect-wsl-logs.ps1 -Dump
```

Kod tamamlandığında günlük dosyasına giden yolun çıktısını verecektir.

#### Otomatik çökme dökümü toplamayı etkinleştirme

Çökmeniz düzensizse veya yeniden oluşturması zorsa, lütfen bu davranışa ilişkin günlükleri yakalamak için otomatik çökme dökümlerini etkinleştirin:

```
md C:\crashes
reg.exe add "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /f
reg.exe add "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpFolder /t REG_EXPAND_SZ /d C:\crashes /f
reg.exe add "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpType /t REG_DWORD /d 2 /f
```

Daha sonra çökme dökümleri otomatik olarak C:\crashes dosyasına yazılacaktır.

İşiniz bittiğinde, aşağıdaki komutu yükseltilmiş bir komut isteminde çalıştırarak çökme dökümü toplamayı devre dışı bırakabilirsiniz:

```
reg.exe delete "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /f
```

### 12) wslservice için Time Travel Debugging İzleri Toplama

To collect time travel debugging traces:

1) [Windbg Preview'ı yükleyin](https://apps.microsoft.com/store/detail/windbg-preview/9PGJGD53TN86?hl=en-us&gl=us&rtc=1)

2) Yönetici olarak açtığınız komut isteminde `windbgx` komutunu çalıştırarak windbg önizlemesini yönetici olarak açın

3) `file` -> `Attach to process` seçeneğine gidin.

4) `Record with Time Travel Debugging` seçeneğini işaretleyin (sağ altta)

4) `Show processes from all users` seçeneğini işaretleyin (en altta)

5) `wslservice.exe` dosyasını seçin. Not, eğer wslservice.exe çalışmıyorsa, onu şu şekilde başlatabilirsiniz: `wsl.exe -l`

6) `Configure and Record`'a tıklayın (izler için seçtiğiniz klasörü not edin)

7) Sorunu yeniden oluşturun

8) windbg'ye geri dönün ve `Stop and Debug`'a tıklayın

9) İz toplama işlemi tamamlandığında, `Stop Debugging` seçeneğine tıklayın ve Windbg'yi kapatın

10) İzin toplandığı klasöre gidin ve .run dosyasını bulun. Şöyle görünmeli: `wslservice*.run`

11) Bu dosyayı konuyla ilgili paylaşın