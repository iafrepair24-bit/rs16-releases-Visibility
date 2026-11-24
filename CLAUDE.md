# RS16 — aturan proyek

## PENGUKURAN WAKTU DIKUNCI (perintah pemilik, 20 Nov 2026)

Cara pengukuran waktu RS16 sudah dinyatakan bagus oleh pemilik dan **tidak boleh diubah**,
di versi mana pun, kecuali pemilik sendiri memerintahkan perubahan pada bagian ini secara eksplisit.
Permintaan lain (tampilan, menu, tombol, font, history, IMU, dll.) BUKAN izin untuk menyentuh bagian ini.
Versi acuan: `RS16_V36_8/RS16_V36_8.ino` di commit `f10f59a` (branch `esp32-devkit-test`).

Yang dikunci (nama fungsi/konstanta di `RS16_V36_8.ino`, ditandai `[DIKUNCI]` di kode):

- Waktu dari GPS: `gpsTowDeltaMsV30()`, `gpsTowInterpolateV30()` (iTOW u-blox, interpolasi linier antar sampel).
- Penerimaan data: `handleUbxPacketV30()` bagian NAV-PVT dan `gpsOnPvtSampleV40()` — SETIAP paket NAV-PVT
  langsung diproses mesin drag/circuit/logger (tidak boleh kembali ke "sekali per loop").
- Mesin drag: `serviceDragV31()` seluruhnya —
  - jarak = integrasi trapesium kecepatan Doppler (deadband 0,5 km/h), sampel dengan jeda >= 500 ms membatalkan run;
  - start: rollout ON = saat jarak 0,3048 m (1 ft) diinterpolasi; rollout OFF = saat kecepatan melewati 1 km/h diinterpolasi;
  - sebelum timing mulai, kecepatan < 1 km/h membuang jarak pra-start seketika; start hanya dari kondisi diam;
  - checkpoint & finish (jarak atau kecepatan) diinterpolasi di antara dua sampel GPS;
  - berhenti di tengah run: `DRAG_STOP_DISCARD_MS_V40` = 500 ms (dibuang), `DRAG_STOP_SAVE_MS_V40` = 2000 ms (disimpan STOP).
- `finishDragV31()` (perhitungan `totalMs`), `dragSavePartialV37()`.
- Target: `dragFinishDistanceV31()` (1/4 mile 402,336 m, 1/8 mile 201,168 m), `dragTargetsV31()`, `dragFinishSpeedV31()`.
- Run baru dari layar FINISH: logika gerak 2 sampel + putar ulang sampel diam di `serviceDragFinishFreezeV36()`.
- Circuit: perhitungan lap di `serviceCircuitV31()` (interpolasi lintasan garis, lap minimal 10 s, lap 1 dari lintasan pertama).
- Rate GPS: `configureGnssRateMsgsV36()` + `gnssPickRateV40()` (0,04 s / 0,06 s mengikuti GNSS SYSTEM, turun otomatis).

Pengecualian yang SUDAH diizinkan pemilik:
- 24 Nov 2026 — mode cari satelit (`serviceGpsSearchModeV40()`): selama GPS belum fix 3D dan tidak ada run,
  rate modul 1 Hz; begitu fix 3D langsung kembali ke rate normal lewat `gnssPickRateV40()` +
  `configureGnssRateMsgsV36()` (isi kedua fungsi itu tetap tidak diubah). Run selalu diukur di rate normal.

Kalau ada perubahan lain yang terpaksa menyentuh fungsi di atas, BERHENTI dan tanya pemilik dulu.
Setelah setiap perubahan, pastikan isi fungsi-fungsi di atas identik dengan commit `f10f59a`
(misalnya `git diff f10f59a -- RS16_V36_8/RS16_V36_8.ino` lalu cek tidak ada baris di fungsi tersebut yang berubah).

## Lain-lain

- Bahasa komunikasi dengan pemilik: Indonesia. "Kerjakan" = perintah untuk melaksanakan.
- Jangan ubah logo dan bitmap menu utama.
- Hardware: PCB V2 100x55 (ESP32-WROOM-32). IMU U8 = LSM6 (footprint ICM-42688 LGA-14, pin kompatibel).
- Build: board ESP32 Dev Module, Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA)".
