# Relay

Relay, bir [session leader](session-leader.md) tarafından oluşturulan bir WSL2 linux sürecidir. Görevi, kullanıcı adına bir linux süreci oluşturmak ve çıktısını Windows'a geri iletmektir. 

## Bir kullanıcı süreci oluşturma

Bir `LxInitMessageCreateProcessUtilityVm` mesajı bir [session leader](session-leader.md)'a gönderildiğinde bir relay oluşturulur. Oluşturulduktan sonra, `relay` [wslservice.exe](wslservice.exe.md) ile birden fazla `hvsocket` kanalı oluşturur.

Bu kanallar aşağıdakiler için kullanılır:

- Standart dosya tanımlayıcılarını (stdin, stdout, stderr) aktarma
- Terminal hakkında bilgi aktarımı (örneğin terminal penceresi Windows'tan yeniden boyutlandırıldığında)
- Linux işleminden çıkıldığında Windows'a bildir

Bu kanallar yapılandırıldıktan sonra, `relay` forks() iki sürece ayrılır: 

- Child'ın standart dosya tanımlayıcılarını okuyup yazacak ve Windows'a iletecek olan parent
- Exec()` işlevini çağıran ve kullanıcı sürecini başlatan child