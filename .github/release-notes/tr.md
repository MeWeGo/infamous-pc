**Yenilikler**

- **3D Görevlerde Çökme Düzeltildi (SRT Walker Overrun)**: SRT walker sinyal işleyicisindeki (`flatten_extended_userdata_pass.cpp`) tampon sınır hesaplama hatası ve decode uzunluğu düzeltilerek 3D oyun dünyasına ve görevlere geçişte oluşan bellek erişim ihlali (access violation) giderildi.
- **Pencere Görünürlüğü ve Ekran Odaklanması**: SDL penceresi oluşturulurken pencere zorla görünür kılınarak ve ön plana getirilerek miras alınan `SW_HIDE` durumlarında pencerenin gizli kalması engellendi.
- **Tek Kuyruklu Cihazlarda Readback Çökmesi Düzeltildi**: Readback kuyruk kaydı `IsAvailable()` kontrolüne bağlanarak tek kuyruklu grafik kartlarında null kuyruk çökmeleri önlendi.
- **Wave64 Atomik Tekdüzelik ve LDS Bayatlık Düzeltmesi**: Atomik komutlar için lane-uniform sınıflandırması düzeltilerek döngü kilitlenmeleri engellendi; `ReadFirstLane` içindeki eski LDS geçerlilik bayrağı temizlendi.
- **Buffer Cache Veri Yarışması Giderildi**: `RegionBits` okuma ve yazma işlemleri atomik hale getirilerek thread-safety sağlandı.
- **Async Compute Kuyruk İndeksi Çakışması Düzeltildi**: Hem async hem standart compute kuyruğu etkinken indeks 1 üzerinde oluşan çakışma giderildi.
- **Pipeline Eşzamanlılığı**: `Handle()` ve `SwapInOptimized()` fonksiyonları mutex ile korunarak pipeline yarışmaları önlendi.
- **CacheStorage ve Sinyal Yönetimi Düzeltildi**: Boş cache dizini silme çökmeleri ve Windows sinyal işleyicisindeki C++ istisna (`0xE06D7363`) hatası düzeltildi.
