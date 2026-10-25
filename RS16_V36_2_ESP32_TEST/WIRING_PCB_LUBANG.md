# RS16 V36.2 — Wiring Test PCB Lubang (ESP32 DevKit 30 pin)

Firmware: `RS16_V36_2_ESP32_TEST.ino` dengan `RS16_ESP32_TEST = 1`.
Wiring mengikuti **PCB utama V2 100x55** (`RS16_V2_KICAD_100x55`). Semua GPIO sama dengan V2,
kecuali tombol: di V2 tombol lewat PCF8574, di sini tombol langsung ke GPIO.

Arduino IDE: board **ESP32 Dev Module**, Serial Monitor **115200**. Kalau port COM tidak muncul,
install driver **CH340**.

---

## 1. Daftar komponen

| Jumlah | Komponen | Untuk |
|---|---|---|
| 1 | ESP32 DevKit 30 pin (hitam, USB-C) | Otak |
| 1 | LCD 2.8" ILI9341 SPI 320x240 | Layar |
| 1 | GPS MG-902 | GNSS 10/25 Hz |
| 1 | MPU6050 (modul GY-521) | IMU / G-meter |
| 1 | Board charger + boost 5V (atau TP4056 dengan proteksi + MT3608 di-set 5,0 V) | Cas dan daya 5V |
| 1 | Baterai Li-ion 1S 18650 2300 mAh + holder | Daya |
| 1 | Slide switch | ON/OFF utama |
| 6 | Tactile switch | Tombol UP, DOWN, OK, BACK, RIGHT, POWER |
| 1 | Buzzer pasif | Bunyi |
| 1 | Transistor NPN 2N2222 / S8050 | Driver buzzer |
| 1 | Dioda 1N4148 / BAT54 | Proteksi buzzer |
| 2 | **Resistor 100k 1%** | **Pembaca baterai** |
| 3 | Resistor 10k | Pull-up tombol BACK, RIGHT, POWER |
| 1 | Resistor 1k | Basis transistor buzzer |
| 1 | Resistor 15 Ω | Seri buzzer |
| 3 | **Kapasitor keramik 100nF (104)** | **Pembaca baterai**, GPS, 5V |
| 1 | Kapasitor 10µF | GPS |
| 1 | Elco 470µF 10V | Bulk 5V (anti reset saat WiFi) |
| — | PCB lubang, header female 15 pin x2, kabel | Rakitan |

---

## 2. Tata letak di PCB lubang

- DevKit dipasang lewat **header female** (bisa dicabut).
- Buat 3 jalur bus memanjang: **GND**, **3V3** (dari pin 3V3 DevKit), **5V** (dari output charger lewat saklar ke pin VIN).
- Kabel SPI ke LCD (D18, D19, D23, D27, D32, D33) **sependek mungkin, idealnya < 10 cm**.
- Kapasitor dipasang **dekat** komponen yang dilayani.

---

## 3. Daya

```
Baterai 18650 (+/-) ── board charger + boost 5V ── OUT 5V ── [saklar] ──┬── VIN DevKit
                                                                        ├── VCC GPS
                                                                        ├── (+) elco 470µF, (−) ke GND
                                                                        └── 100nF ke GND
GND charger ──────────────────────────────────────────────────────────────── GND bus
```

- Baterai **tidak boleh langsung** ke VIN (3,0–4,2 V terlalu rendah untuk regulator DevKit).
- Elco: kaki bergaris putih (−) ke GND.
- Saat upload lewat USB: **saklar OFF** dulu.

---

## 4. Pembaca baterai (VP / GPIO36) — sama dengan R12/R13/C13 di V2

```
BAT+ (langsung dari kutub + baterai) ──[100k 1%]──┬──────── VP (GPIO36)
                                                  │
                                             [100k 1%]     [100nF]
                                                  │           │
GND ──────────────────────────────────────────────┴───────────┘
```

- Ambil dari **BAT+ baterai**, bukan dari output 5V boost.
- 100nF dipasang **dekat pin VP**.
- Kedua resistor harus sama (firmware mengalikan x2). Tegangan maks di VP = 4,2 ÷ 2 = 2,1 V (aman).
- Arus bocor ≈ 0,02 mA (boleh diabaikan).
- **Test tanpa baterai (USB saja): VP ke GND**, supaya board tidak mati sendiri karena terbaca baterai kritis.
- Kalibrasi: bandingkan `bat=` di Serial Monitor dengan multimeter. Selisih > 0,05 V → faktor di firmware disesuaikan.

---

## 5. LCD 2.8" ILI9341

| Pin LCD | DevKit |
|---|---|
| VCC | 3V3 |
| GND | GND |
| CS | **D27** |
| RESET | **D33** |
| DC | **D32** |
| SDI (MOSI) | **D23** |
| SCK | **D18** |
| LED | **3V3** (selalu nyala) — atau lewat transistor dikontrol **D14** seperti V2 |
| SDO (MISO) | **D19** |

Pin touch (T_xxx) dan pin SD di modul LCD: kosongkan.

---

## 6. GPS MG-902 — urutan sama dengan konektor J_GPS1 di V2

| Kabel GPS | DevKit |
|---|---|
| GND | GND |
| VCC | **5V** |
| RX GPS | **D16** (ESP TX) |
| TX GPS | **D17** (ESP RX) |
| SCL / SDA (jika ada) | D22 / D21, atau kosongkan |

Pasang **10µF + 100nF** dari VCC GPS ke GND, dekat konektor GPS.

---

## 7. MPU6050 (GY-521) — sama dengan header J_I2C1 di V2

| GY-521 | DevKit |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL | **D22** |
| SDA | **D21** |
| AD0, INT, XDA, XCL | kosongkan (alamat 0x68) |

Setelah terpasang datar: kalibrasi nol IMU dari menu, atau ketik `imu zero` di Serial Monitor.
Sumbu X = maju/mundur, Y = kiri/kanan.

---

## 8. Tombol (6 tactile switch)

Kaki satu ke GPIO, kaki lain ke GND. Pakai kaki **diagonal** tactile switch.

| Tombol | DevKit | Pull-up |
|---|---|---|
| UP | **D13** | internal (tanpa resistor) |
| DOWN | **D25** | internal (tanpa resistor) |
| OK | **D5** | internal (tanpa resistor) |
| BACK / LEFT | **D35** | **10k ke 3V3 (wajib)** |
| RIGHT | **VN** (GPIO39) | **10k ke 3V3 (wajib)** |
| POWER | **D34** | **10k ke 3V3 (wajib)** |

```
3V3 ──[10k]──┬── D34 / D35 / VN
             └── tombol ── GND
```

POWER: tekan singkat = layar off/on, tahan 2 detik = power off (deep sleep). Tekan POWER untuk menyalakan lagi.
Deep sleep tidak memutus GPS/LCD — untuk benar-benar mati pakai **slide switch**.
Jangan tahan tombol OK (D5) saat boot/upload.

---

## 9. Buzzer — sama dengan R10/Q1 di V2

```
D15 ──[1k]── Basis 2N2222/S8050
             Emitor  ── GND
             Kolektor ── (−) buzzer
5V ──[15Ω]── (+) buzzer
Dioda 1N4148/BAT54 paralel buzzer: katoda (garis) ke (+) buzzer
```

---

## 10. Pin cadangan & pin yang JANGAN disambung

| Pin | Status |
|---|---|
| D4 | Cadangan LED GPS (sesuai V2, belum dipakai firmware) |
| D26 | Cadangan microSD CS (sesuai V2, belum dipakai firmware) |
| **D12, D2** | **Jangan disambung** (pin strapping boot) |
| **TX0, RX0** | **Jangan disambung** (USB upload / Serial Monitor) |

---

## 11. Ringkasan per pin DevKit

| Kiri (atas → bawah) | Fungsi | Kanan (atas → bawah) | Fungsi |
|---|---|---|---|
| EN | — | D23 | LCD MOSI |
| VP | Baterai | D22 | MPU/GPS SCL |
| VN | Tombol RIGHT (+10k) | TX0 | **kosong** |
| D34 | Tombol POWER (+10k) | RX0 | **kosong** |
| D35 | Tombol BACK (+10k) | D21 | MPU/GPS SDA |
| D32 | LCD DC | D19 | LCD MISO |
| D33 | LCD RST | D18 | LCD SCK |
| D25 | Tombol DOWN | D5 | Tombol OK |
| D26 | (cadangan SD CS) | D17 | TX GPS |
| D27 | LCD CS | D16 | RX GPS |
| D14 | Backlight enable | D4 | (cadangan LED GPS) |
| D12 | **kosong** | D2 | **kosong** |
| D13 | Tombol UP | D15 | Buzzer |
| GND | GND bus | GND | GND bus |
| VIN | 5V dari charger | 3V3 | LCD, MPU, pull-up |

---

## 12. Urutan rakit & test

Rakit bertahap, lihat baris `BOOT>` di Serial Monitor (115200) untuk tahu langkah yang macet.

1. **Daya** — ukur VIN ≈ 5 V, 3V3 ≈ 3,3 V; pastikan 3V3–GND tidak short (mode bunyi multimeter).
2. **LCD** — upload firmware, logo RS16 muncul (`BOOT> 13 LCD di-init`).
3. **Tombol** — navigasi menu UP/DOWN/OK/BACK (`BOOT> 06 tombol GPIO ...`).
4. **MPU6050** — `BOOT> 11 IMU = MPU6050 (TERDETEKSI)`.
5. **Baterai** — `bat=` di baris `ALIVE` sama dengan multimeter.
6. **GPS** — di luar ruangan; ketik `status`, target ≈ 25 Hz. Menu GNSS → GNSS STATUS untuk bar sinyal.

Untuk perbandingan dengan Qstarz: set **GNSS SYSTEM = GPS + GLONASS** dan **RATE = 25 HZ** di kedua alat.
