/*
  RS16 TES TOMBOL -- PCB V2 100x55
  Sketch kecil untuk mengecek 6 tombol lewat Serial Monitor (115200), terpisah dari program utama.

  Susunan tombol (dilihat dari depan, LED di kanan atas):
    KIRI : atas = START   | tengah = UP    | bawah = DOWN
    KANAN: atas = POWER   | tengah = RIGHT | bawah = LEFT

  Cara pakai:
    1. Upload sketch ini (Board: ESP32 Dev Module).
    2. Buka Serial Monitor 115200.
    3. Tekan tombol satu per satu -> nama & posisinya muncul.
    4. Setelah selesai, upload lagi program utama RS16_V36_8.

  Jalur: PCF8574 / PCF8574A (I2C SDA 21, SCL 22, 100 kHz), INT ke GPIO13.
  Bit PCF: P0 POWER (lewat Q4, logika terbalik), P1 kanan tengah, P2 kanan bawah,
           P3 kiri atas, P4 kiri bawah, P5 kiri tengah.
*/
#include <Arduino.h>
#include <Wire.h>

#define SDA_PIN 21
#define SCL_PIN 22
#define PCF_INT_PIN 13
#define TFT_BL_PIN 14   // lampu latar dimatikan supaya layar tidak putih selama tes

struct Tombol { uint8_t bit; bool aktifHigh; const char* nama; const char* posisi; };
static const Tombol TOMBOL[6] = {
  {3, false, "START", "KIRI ATAS"},
  {5, false, "UP",    "KIRI TENGAH"},
  {4, false, "DOWN",  "KIRI BAWAH"},
  {0, true,  "POWER", "KANAN ATAS"},
  {1, false, "RIGHT", "KANAN TENGAH"},
  {2, false, "LEFT",  "KANAN BAWAH"},
};

static uint8_t pcfAddr = 0;
static bool ditekanLama[6] = {false};
static uint32_t mulaiTekan[6] = {0};
static bool pernahDitekan[6] = {false};

static int bacaPcf() {
  if (Wire.requestFrom((int)pcfAddr, 1) != 1) return -1;
  return Wire.read();
}

static bool ditekan(uint8_t b, uint8_t i) {
  const bool level = (b >> TOMBOL[i].bit) & 0x01;
  return TOMBOL[i].aktifHigh ? level : !level;   // tombol biasa: LOW = ditekan
}

static void cetakRingkasan() {
  Serial.print("SUDAH DICOBA: ");
  for (uint8_t i = 0; i < 6; i++) {
    Serial.printf("%s=%s  ", TOMBOL[i].nama, pernahDitekan[i] ? "OK" : "--");
  }
  Serial.println();
}

void setup() {
  pinMode(TFT_BL_PIN, OUTPUT); digitalWrite(TFT_BL_PIN, LOW);
  pinMode(PCF_INT_PIN, INPUT);
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== RS16 TES TOMBOL ===");
  Wire.begin(SDA_PIN, SCL_PIN, 100000);

  // cari PCF8574A (0x38-0x3F) lalu PCF8574 (0x20-0x27)
  const uint8_t base[2] = {0x38, 0x20};
  for (uint8_t b = 0; b < 2 && !pcfAddr; b++) {
    for (uint8_t a = 0; a < 8 && !pcfAddr; a++) {
      Wire.beginTransmission(base[b] + a);
      Wire.write(0xFF);   // semua pin jadi input
      if (Wire.endTransmission() == 0) pcfAddr = base[b] + a;
    }
  }
  if (!pcfAddr) {
    Serial.println("PCF8574 TIDAK DITEMUKAN. Alamat I2C yang menjawab:");
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) Serial.printf("  0x%02X\n", a);
    }
    Serial.println("Cek solderan U5 (PCF8574), R pull-up SDA/SCL, dan 3V3.");
    return;
  }
  Serial.printf("PCF8574%s ditemukan di 0x%02X\n", pcfAddr >= 0x38 ? "A" : "", pcfAddr);
  const int b = bacaPcf();
  Serial.printf("Byte awal = 0x%02X (tanpa tombol ditekan normalnya 0xFE)\n", b & 0xFF);
  Serial.println("Tekan tombol satu per satu...");
  Serial.println();
}

void loop() {
  if (!pcfAddr) { delay(1000); return; }
  static int lastByte = -2;
  static uint32_t lastPrint = 0;
  const int b = bacaPcf();
  if (b < 0) {
    if (millis() - lastPrint > 1000) { lastPrint = millis(); Serial.println("GAGAL BACA I2C"); }
    delay(20);
    return;
  }
  for (uint8_t i = 0; i < 6; i++) {
    const bool p = ditekan((uint8_t)b, i);
    if (p && !ditekanLama[i]) {
      mulaiTekan[i] = millis();
      pernahDitekan[i] = true;
      Serial.printf(">> %-5s DITEKAN   (%s)   byte=0x%02X  INT=%s\n",
                    TOMBOL[i].nama, TOMBOL[i].posisi, b & 0xFF,
                    digitalRead(PCF_INT_PIN) == LOW ? "LOW" : "HIGH");
    } else if (!p && ditekanLama[i]) {
      Serial.printf("   %-5s dilepas   (%lu ms)\n", TOMBOL[i].nama, (unsigned long)(millis() - mulaiTekan[i]));
      cetakRingkasan();
    }
    ditekanLama[i] = p;
  }
  if (b != lastByte && lastByte != -2) {
    uint8_t n = 0; for (uint8_t i = 0; i < 6; i++) if (ditekanLama[i]) n++;
    if (n >= 2) Serial.printf("   (%u tombol ditekan bersamaan)\n", n);
  }
  lastByte = b;
  delay(10);
}
