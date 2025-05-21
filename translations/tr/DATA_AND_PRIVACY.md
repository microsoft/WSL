# WSL Verileri ve Gizlilik

## Genel Bakış

WSL, diğer Windows bileşenleri gibi Windows telemetrisini kullanarak tanılama verilerini toplar. Bunu Windows Ayarları'nı açıp Gizlilik ve Güvenlik -> Tanılama ve Geri Bildirim'e gidip 'Tanılama verileri'ni devre dışı bırakarak devre dışı bırakabilirsiniz. Ayrıca bu menüde 'Tanılama verilerini görüntüle' seçeneğini kullanarak gönderdiğiniz tüm tanılama verilerini görüntüleyebilirsiniz. 

Daha fazla bilgi için lütfen [Microsoft gizlilik bildirimi](https://www.microsoft.com/privacy/privacystatement) adresini okuyun.

## WSL Ne Toplar?

1. Kullanım
   - WSL'de en sık hangi özelliklerin ve ayarların kullanıldığını anlamak, zamanımızı ve enerjimizi nereye odaklayacağımız konusunda karar vermemize yardımcı olur.
2. İstikrar
   - Hataları ve sistem çökmelerini izlemek, en acil sorunlara öncelik vermemize yardımcı olur.
3. Performans
   - WSL'nin performansını değerlendirmek, hangi çalışma zamanlarının / bileşenlerin yavaşlamaya neden olabileceğini anlamamızı sağlar. Bu, size hızlı ve etkili bir WSL sağlama konusundaki kararlılığımızı destekler. 

Bu deponun kaynak kodunda `WSL_LOG_TELEMETRY` çağrılarını arayarak WSL telemetri olaylarını arayabilirsiniz.