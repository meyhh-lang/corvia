#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Firebase_ESP_Client.h>
#include "MAX30105.h"
#include "heartRate.h"

// ================= ISIKAN KREDENSIAL DI SINI =================
#define WIFI_SSID "Raza"
#define WIFI_PASSWORD "sukamatcha"

#define DATABASE_URL "corvia-381fd-default-rtdb.asia-southeast1.firebasedatabase.app"
#define DATABASE_SECRET "mWBT6jsO4L0rfZdG9TAI6F13muuTQtoAiNF7Mb0Q"
// =============================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

MAX30105 particleSensor;

const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;

float beatsPerMinute;
int beatAvg = 0;
int spo2Val = 0;

bool isMeasuring = false;
unsigned long measureStartTime = 0;
const unsigned long MEASURE_DURATION = 10000; // 10 detik

unsigned long lastFirebaseUpload = 0;
const unsigned long FIREBASE_INTERVAL = 2000; 

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Variable pendukung deteksi peak BPM baru
static long lastIR = 0;
static bool peakDetected = false;

const unsigned char PROGMEM heart_bmp[] = {
  0b00000000, 0b00000000,
  0b00011100, 0b00111000,
  0b01111110, 0b01111110,
  0b11111111, 0b11111111,
  0b11111111, 0b11111111,
  0b11111111, 0b11111111,
  0b01111111, 0b11111000,
  0b00111111, 0b11111000,
  0b00011111, 0b11100000,
  0b00001111, 0b11100000,
  0b00000111, 0b11000000,
  0b00000011, 0b10000000,
  0b00000001, 0b00000000,
  0b00000000, 0b00000000,
  0b00000000, 0b00000000,
  0b00000000, 0b00000000
};

void setup() {
  Serial.begin(115200);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED SSD1306 Gagal!"));
    for (;;);
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println("Menghubungkan Wi-Fi...");
  display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("MAX30102 Tidak Ditemukan!");
    display.display();
    while (1);
  }

  // --- REGION KONFIGURASI SENSOR DENGAN SAMPEL PRESISI ---
  byte ledBrightness = 0x24; // Kecerahan LED sedang agar aliran darah tidak silau
  byte sampleAverage = 4;    // Average 4 samples
  byte ledMode = 2;          // Red + IR
  int sampleRate = 100;      // 100Hz
  int pulseWidth = 411;      // Lebar pulsa 411us
  int adcRange = 4096;       // 12-bit ADC

  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
}

void loop() {
  long irValue = particleSensor.getIR();
  long redValue = particleSensor.getRed();

  if (irValue > 50000) {

    // Set Awal Pengukuran
    if (!isMeasuring) {
      isMeasuring = true;
      measureStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(20, 20);
      display.println("Mengukur...");
      display.setCursor(10, 40);
      display.println("Mohon Tunggu...");
      display.display();
    }

    // --- LOGIKA DETEKSI PUNCKAK PULSA BPM DENGAN THRESHOLD ---
    if (irValue > 60000 && (irValue - lastIR) > 300 && !peakDetected) {
      peakDetected = true;
      long delta = millis() - lastBeat;
      lastBeat = millis();

      if (lastBeat > 0 && delta > 300 && delta < 2000) { 
        beatsPerMinute = 60000.0 / delta;

        if (beatsPerMinute < 200 && beatsPerMinute > 40) {
          rates[rateSpot++] = (byte)beatsPerMinute;
          rateSpot %= RATE_SIZE;

          beatAvg = 0;
          for (byte x = 0; x < RATE_SIZE; x++) {
            beatAvg += rates[x];
          }
          beatAvg /= RATE_SIZE;
        }
      }
    } 
    
    if ((lastIR - irValue) > 300) {
      peakDetected = false; 
    }
    lastIR = irValue;

    // Estimasi Kalkulasi SpO2
    if (irValue > 0) {
      double ratio = (double)redValue / (double)irValue;
      spo2Val = (int)(110 - (18 * ratio));
      if (spo2Val > 100) spo2Val = 100;
      if (spo2Val < 70) spo2Val = 70;
    }

    // Tampilkan Hasil Hanya Setelah 10 Detik
    if (millis() - measureStartTime >= MEASURE_DURATION) {
      
      String statusBPM = "";
      if (beatAvg < 60) statusBPM = "BRADIKARDIA";
      else if (beatAvg <= 100) statusBPM = "NORMAL";
      else statusBPM = "TAKIKARDIA";

      String statusSpO2 = "";
      if (spo2Val >= 95) statusSpO2 = "NORMAL";
      else if (spo2Val >= 90) statusSpO2 = "RINGAN";
      else statusSpO2 = "HIPOKSIA";

      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.println("-- MONITOR KESEHATAN --");

      display.drawBitmap(0, 14, heart_bmp, 16, 16, SSD1306_WHITE);
      display.setCursor(22, 14);
      display.setTextSize(2);
      display.print("BPM: ");
      display.println(beatAvg);

      display.setCursor(0, 34);
      display.setTextSize(1);
      display.print("SpO2 : ");
      display.print(spo2Val);
      display.println(" %");

      display.setCursor(0, 46);
      display.print("BPM  : ");
      display.println(statusBPM);

      display.setCursor(0, 56);
      display.print("SpO2 : ");
      display.println(statusSpO2);

      display.display();

      // Kirim Data ke Firebase (Path Disesuaikan dengan Konsol)
      if (millis() - lastFirebaseUpload >= FIREBASE_INTERVAL) {
        lastFirebaseUpload = millis();

        if (Firebase.ready()) {
          Firebase.RTDB.setInt(&fbdo, "/monitoring/realtime/bpm", beatAvg);
          Firebase.RTDB.setInt(&fbdo, "/monitoring/realtime/spo2", spo2Val);
          Firebase.RTDB.setString(&fbdo, "/monitoring/realtime/status", statusBPM);
        }
      }
    }

  } else {
    // Reset saat jari dilepas
    isMeasuring = false;
    beatAvg = 0;
    spo2Val = 0;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 20);
    display.println("Tempelkan Jari Imup Anda :D");
    display.setCursor(30, 40);
    display.println("pada Sensor");
    display.display();
  }
}