/*
  RS16 TES IMU -- PCB V2 100x55, chip U8 (ICM-42688-P, atau LSM6DS3 di footprint yang sama)
  Sketch kecil KHUSUS mencari kenapa IMU tidak terdeteksi. Program utama tidak disentuh.

  Cara pakai:
    1. Upload sketch ini (Board: ESP32 Dev Module).
    2. Buka Serial Monitor 115200, tunggu laporan muncul (diulang tiap 5 detik).
    3. Kirim seluruh teks laporan ke Claude.
    4. Setelah selesai, upload lagi program utama RS16_V36_8.
  Sebaiknya microSD DILEPAS dulu saat tes (kartu SD memakai jalur MISO yang sama).

  Yang dicek:
    A. Jalur: pull-up CS (R16 10k) dan pull-up MISO (R33 10k) -- apakah ada yang korsleting ke GND.
    B. SPI: baca register WHO_AM_I di 4 mode SPI x 3 kecepatan.
    C. I2C lewat kaki yang sama (SCK = SCL, MOSI = SDA): saat CS tidak aktif chip juga menjawab I2C.
       Kalau I2C menjawab tapi SPI tidak -> chip HIDUP, masalah di kaki CS atau MISO (solderan).
    D. Kalau terdeteksi: baca data accelerometer supaya terlihat chip benar-benar bekerja.

  Pin (sesuai PCB V2): SCK 18, MISO 19, MOSI 23, CS IMU 5, INT1 34.
  CS lain dibuat HIGH: LCD 27, flash 25, microSD 26. Lampu latar (14) dimatikan.
*/
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

#define PIN_SCK   18
#define PIN_MISO  19
#define PIN_MOSI  23
#define PIN_CS    5
#define PIN_INT1  34
#define PIN_TFT_CS   27
#define PIN_FLASH_CS 25
#define PIN_SD_CS    26
#define PIN_TFT_BL   14

SPIClass spi(VSPI);

static bool icmWho(uint8_t w){ return w==0x47 || w==0xDB || w==0x42; }   // ICM-42688-P / -V / ICM-42605
static bool lsmWho(uint8_t w){ return w==0x69 || w==0x6A || w==0x6B || w==0x6C || w==0x70 || w==0x71; }
static const char* chipName(uint8_t w){
  switch(w){
    case 0x47: return "ICM-42688-P"; case 0xDB: return "ICM-42688-V"; case 0x42: return "ICM-42605";
    case 0x69: return "LSM6DS3"; case 0x6A: return "LSM6DS3TR-C/LSM6DSL"; case 0x6B: return "LSM6DSR/ISM330";
    case 0x6C: return "LSM6DSO"; case 0x70: case 0x71: return "LSM6DSV";
    default: return "?";
  }
}

static void allCsHigh(){
  digitalWrite(PIN_CS,HIGH); digitalWrite(PIN_TFT_CS,HIGH); digitalWrite(PIN_FLASH_CS,HIGH); digitalWrite(PIN_SD_CS,HIGH);
}

static uint8_t spiRead(uint8_t reg, uint32_t hz, uint8_t mode){
  spi.beginTransaction(SPISettings(hz, MSBFIRST, mode));
  digitalWrite(PIN_CS, LOW); delayMicroseconds(2);
  spi.transfer(reg | 0x80);
  const uint8_t v = spi.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
  delayMicroseconds(50);
  return v;
}
static void spiWrite(uint8_t reg, uint8_t val, uint32_t hz, uint8_t mode){
  spi.beginTransaction(SPISettings(hz, MSBFIRST, mode));
  digitalWrite(PIN_CS, LOW); delayMicroseconds(2);
  spi.transfer(reg & 0x7F); spi.transfer(val);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
  delayMicroseconds(50);
}

static void spiStart(){
  pinMode(PIN_CS,OUTPUT); pinMode(PIN_TFT_CS,OUTPUT); pinMode(PIN_FLASH_CS,OUTPUT); pinMode(PIN_SD_CS,OUTPUT);
  allCsHigh();
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  // 80+ clock dengan semua CS HIGH: kartu microSD melepas jalur MISO
  spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for(uint8_t i=0;i<12;i++) spi.transfer(0xFF);
  spi.endTransaction();
}

// ---------------- A. cek jalur ----------------
static int cekPullup(uint8_t pin){
  // pull-down internal (+-45k) lawan pull-up PCB 10k: normal terbaca HIGH
  pinMode(pin, INPUT_PULLDOWN); delay(3);
  const int v = digitalRead(pin);
  return v;
}

// ---------------- C. I2C lewat SCK/MOSI ----------------
static bool i2cProbe(uint8_t addr, uint8_t reg, uint8_t* out){
  Wire1.beginTransmission(addr); Wire1.write(reg);
  if(Wire1.endTransmission(false) != 0) return false;
  if(Wire1.requestFrom((int)addr, 1) != 1) return false;
  *out = Wire1.read(); return true;
}

static uint8_t foundWho = 0, foundMode = 0xFF; static uint32_t foundHz = 0;

static void laporan(){
  Serial.println();
  Serial.println("================ RS16 TES IMU ================");
  pinMode(PIN_TFT_BL,OUTPUT); digitalWrite(PIN_TFT_BL,LOW);

  // A. jalur
  spi.end();
  pinMode(PIN_TFT_CS,OUTPUT); pinMode(PIN_FLASH_CS,OUTPUT); pinMode(PIN_SD_CS,OUTPUT);
  digitalWrite(PIN_TFT_CS,HIGH); digitalWrite(PIN_FLASH_CS,HIGH); digitalWrite(PIN_SD_CS,HIGH);
  const int csLine = cekPullup(PIN_CS);
  const int misoLine = cekPullup(PIN_MISO);
  pinMode(PIN_INT1, INPUT);
  const int int1 = digitalRead(PIN_INT1);
  Serial.println("[A] JALUR");
  Serial.printf("    CS IMU (GPIO5, pull-up R16) : %s\n", csLine ? "HIGH = normal" : "LOW  = R16 hilang / CS korsleting ke GND");
  Serial.printf("    MISO   (GPIO19, pull-up R33): %s\n", misoLine ? "HIGH = normal" : "LOW  = MISO ditarik ke GND (korsleting / kartu SD / chip rusak)");
  Serial.printf("    INT1   (GPIO34)             : %s\n", int1 ? "HIGH" : "LOW");

  // B. SPI
  spiStart();
  Serial.println("[B] SPI  WHO_AM_I  (ICM reg 0x75 / LSM reg 0x0F)");
  static const uint8_t modes[4] = {SPI_MODE0, SPI_MODE1, SPI_MODE2, SPI_MODE3};
  static const uint32_t hzs[3] = {100000, 500000, 1000000};
  foundWho = 0; foundMode = 0xFF;
  bool semuaFF = true, semua00 = true;
  for(uint8_t m=0;m<4;m++){
    Serial.printf("    MODE%u:", m);
    for(uint8_t h=0;h<3;h++){
      uint8_t a = spiRead(0x75, hzs[h], modes[m]); a = spiRead(0x75, hzs[h], modes[m]);   // baca 2x, pakai yang kedua
      const uint8_t b = spiRead(0x0F, hzs[h], modes[m]);
      Serial.printf("  %4luk ICM=0x%02X LSM=0x%02X", (unsigned long)(hzs[h]/1000), a, b);
      if(a!=0xFF||b!=0xFF) semuaFF=false;
      if(a!=0x00||b!=0x00) semua00=false;
      if(!foundWho && (icmWho(a)||lsmWho(b))){ foundWho = icmWho(a)?a:b; foundMode = m; foundHz = hzs[h]; }
    }
    Serial.println();
  }

  // C. I2C lewat kaki SCK/MOSI (CS IMU HIGH = mode I2C)
  spi.end();
  digitalWrite(PIN_CS,HIGH); pinMode(PIN_CS,OUTPUT); digitalWrite(PIN_CS,HIGH);
  Wire1.begin(PIN_MOSI, PIN_SCK, 20000);   // SDA = MOSI, SCL = SCK, pull-up internal ESP32
  Serial.println("[C] I2C lewat kaki SCK(SCL) / MOSI(SDA)");
  bool i2cHit = false; uint8_t i2cWho = 0, i2cAddr = 0;
  const uint8_t addrs[4] = {0x68, 0x69, 0x6A, 0x6B};
  for(uint8_t i=0;i<4;i++){
    uint8_t w1=0, w2=0;
    const bool r1 = i2cProbe(addrs[i], 0x75, &w1);
    const bool r2 = i2cProbe(addrs[i], 0x0F, &w2);
    if(r1||r2){
      Serial.printf("    0x%02X menjawab: reg75=0x%02X reg0F=0x%02X\n", addrs[i], w1, w2);
      if(icmWho(w1)){ i2cHit=true; i2cWho=w1; i2cAddr=addrs[i]; }
      else if(lsmWho(w2)){ i2cHit=true; i2cWho=w2; i2cAddr=addrs[i]; }
    }
  }
  if(!i2cHit) Serial.println("    tidak ada IMU yang menjawab lewat I2C");
  Wire1.end();

  // D. data accelerometer
  spiStart();
  if(foundWho){
    Serial.printf("[D] TERDETEKSI: %s (WHO 0x%02X) lewat SPI MODE%u %lu kHz\n", chipName(foundWho), foundWho, foundMode, (unsigned long)(foundHz/1000));
    const uint8_t md = modes[foundMode];
    if(icmWho(foundWho)){
      spiWrite(0x11, 0x01, 500000, md); delay(50);          // soft reset
      spiWrite(0x4C, 0x33, 500000, md);                     // matikan I2C
      spiWrite(0x4E, 0x0F, 500000, md); delay(50);          // accel + gyro aktif
      for(uint8_t k=0;k<5;k++){
        int16_t ax=(int16_t)((spiRead(0x1F,500000,md)<<8)|spiRead(0x20,500000,md));
        int16_t ay=(int16_t)((spiRead(0x21,500000,md)<<8)|spiRead(0x22,500000,md));
        int16_t az=(int16_t)((spiRead(0x23,500000,md)<<8)|spiRead(0x24,500000,md));
        Serial.printf("    ACC  X=%+.2f  Y=%+.2f  Z=%+.2f g\n", ax/2048.0f, ay/2048.0f, az/2048.0f);   // +-16 g default
        delay(200);
      }
    }else{
      spiWrite(0x12, 0x44, 500000, md);                     // BDU + auto increment
      spiWrite(0x10, 0x60, 500000, md); delay(50);          // accel 416 Hz +-2 g
      for(uint8_t k=0;k<5;k++){
        int16_t ax=(int16_t)((spiRead(0x29,500000,md)<<8)|spiRead(0x28,500000,md));
        int16_t ay=(int16_t)((spiRead(0x2B,500000,md)<<8)|spiRead(0x2A,500000,md));
        int16_t az=(int16_t)((spiRead(0x2D,500000,md)<<8)|spiRead(0x2C,500000,md));
        Serial.printf("    ACC  X=%+.2f  Y=%+.2f  Z=%+.2f g\n", ax*0.061f/1000.0f, ay*0.061f/1000.0f, az*0.061f/1000.0f);
        delay(200);
      }
    }
    Serial.println("    Diam di meja: salah satu sumbu harus +-1.00 g, dua lainnya mendekati 0.");
  }

  // KESIMPULAN
  Serial.println("---------------- KESIMPULAN ----------------");
  if(foundWho){
    Serial.println("IMU HIDUP dan terbaca lewat SPI. Kirim laporan ini; program utama akan disesuaikan.");
  }else if(i2cHit){
    Serial.printf("Chip HIDUP (%s di I2C 0x%02X) tapi SPI GAGAL.\n", chipName(i2cWho), i2cAddr);
    Serial.println("-> Cek solderan kaki 12 (CS) dan kaki 1 (SDO/MISO) chip U8, serta R16.");
  }else if(!misoLine){
    Serial.println("MISO tertarik ke GND -> cek korsleting di jalur MISO / chip U8 / slot microSD.");
  }else if(!csLine){
    Serial.println("Jalur CS IMU LOW -> R16 tidak terpasang atau CS (GPIO5) korsleting ke GND.");
  }else if(semuaFF){
    Serial.println("Chip TIDAK MENJAWAB sama sekali (semua 0xFF) lewat SPI maupun I2C.");
    Serial.println("-> Kemungkinan: chip tidak mendapat 3V3, solderan kaki LGA tidak menempel,");
    Serial.println("   chip terpasang terbalik (cek titik kaki 1), atau chip rusak/palsu.");
  }else if(semua00){
    Serial.println("Semua 0x00 -> MISO tertahan LOW saat dibaca. Cek korsleting kaki 1 chip U8.");
  }else{
    Serial.println("Chip menjawab angka lain (lihat bagian B). Kirim laporan ini untuk dianalisa.");
  }
  Serial.println("============================================");
}

void setup(){
  pinMode(PIN_TFT_BL,OUTPUT); digitalWrite(PIN_TFT_BL,LOW);
  Serial.begin(115200);
  delay(500);
  laporan();
}

void loop(){
  static uint32_t t=0;
  if(millis()-t>5000){ t=millis(); laporan(); }
}
