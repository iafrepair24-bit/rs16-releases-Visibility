# Diagnosa USB Android / iPhone (Windows)

Alat bantu **servis & diagnosa** lewat colok USB. Fokus utama: kasus
**Android LCD gelap** tapi mesin masih hidup — supaya bisa dilihat/masuk menu
dari PC. Alat ini **tidak** melakukan bypass kunci / activation lock apa pun.

## 1. Yang perlu diinstal di Windows

| Tool | Untuk apa | Sumber |
|------|-----------|--------|
| **Python 3** | menjalankan script | python.org (centang "Add to PATH") |
| **Android Platform Tools** (`adb`) | deteksi & akses Android | SDK Platform Tools resmi Google |
| **scrcpy** | tampilkan layar HP di PC (LCD gelap) | rilis resmi scrcpy |
| **libimobiledevice** + driver Apple | deteksi iPhone (opsional) | build Windows libimobiledevice / iTunes |

Taruh folder `adb` dan `scrcpy` ke **PATH** Windows, lalu:

```
pip install -r requirements.txt
```

(`colorama` hanya untuk warna di terminal — opsional.)

## 2. Cara pakai

```
python diagnosa.py            # deteksi + tampilkan info
python diagnosa.py --mirror   # deteksi, lalu buka scrcpy jika Android siap
python diagnosa.py --watch    # pantau terus saat colok/cabut
```

Atau klik dua kali **`diagnosa.bat`**.

## 3. Alur untuk Android LCD gelap

1. Colok HP ke PC pakai **kabel data** (bukan kabel charge saja).
2. Jalankan `python diagnosa.py`.
   - Status **`[OK] ... (USB Debugging ON)`** → jalankan `--mirror`, layar HP
     muncul di PC, bisa diklik pakai mouse.
   - Status **`unauthorized`** → tekan **Allow** di layar HP (blind touch, tombol
     kanan-bawah) atau colok **mouse OTG** untuk klik.
   - Tidak terdeteksi → cek bagian **Mode Khusus** di output (EDL/Preloader),
     atau cek kabel/port.

## 4. Catatan

- Jalan di **PC Windows**, bukan di ESP32. Protokol ADB/usbmux terlalu berat
  untuk ESP32-S3.
- Jika nanti mau layar penampil terpisah berbasis ESP32-S3, itu jadi modul
  tambahan yang membaca output script ini — bisa dikerjakan terpisah.
