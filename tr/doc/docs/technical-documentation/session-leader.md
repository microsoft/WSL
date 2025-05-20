# Session leader

Bir session leader, bir `LxInitMessageCreateSession` mesajı aldıktan sonra [init](init.md)'den çatallanan bir linux sürecidir (bkz. `src/linux/init.cpp`)

## Linux süreçleri oluşturma

Oturum liderleri kullanıcı adına linux süreçleri oluşturmak için kullanılır. Her linux session leader bir Windows konsoluyla ilişkilendirilir. 

Bir kullanıcı işlemi oluşturmak için [wslservice.exe](wslservice.exe.md), oluşturulacak işlemle ilgili ayrıntıları içeren bir `LxInitMessageCreateProcess` mesajı (WSL1) veya bir `LxInitMessageCreateProcessUtilityVm` mesajı (WSL2) gönderir:

- Komut satırı
- Geçerli dizin
- Ortam değişkenleri 
- Kullanıcı adı

### WSL1 süreci oluşturma

Bir WSL1 dağıtımında çalışırken, session leader forks() yapar ve kullanıcı linux sürecine `exec()` yapmak için alt süreci kullanır. `Exec()` işlevini çağırmadan önce, child aşağıdaki gibi çeşitli ayarları yapılandırır:

- Kullanıcı ve grup kimliği
- Geçerli dizin
- Standart dosya tanımlayıcıları (stdin, stdout, stderr)

## WSL2 süreci oluşturma

Bir WSL2 dağıtımında çalışırken, oturum liderleri kullanıcı sürecini oluşturmaktan ve çıktısını [wsl.exe](wsl.exe.md)'ye geri iletmekten sorumlu olan bir [relay](relay.md) süreci oluşturmak için forks()