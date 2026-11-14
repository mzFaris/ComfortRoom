#include <DHT.h>
#include <PubSubClient.h>
#include <WiFi.h>

// --- KONFIGURASI PIN (Sesuai Rangkaian Simulasi) ---
#define PIR_PIN 14    // Pin D14: Input sensor PIR
#define DHT_PIN 27    // Pin D27: Input sensor DHT11
#define LDR_PIN 34    // Pin D34 (ADC1): Input analog sensor LDR
#define BUZZER_PIN 26 // Pin D26: Output buzzer
#define LED_PIN 4     // Pin D4: Output LED indikator eksternal

#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// --- KONFIGURASI WIFI & MQTT ---
// Gunakan "Wokwi-GUEST" (tanpa password) untuk simulasi, ganti dengan WiFi Anda
// saat perakitan fisik
const char *ssid = "Wokwi-GUEST";
const char *password = "";
const char *mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// --- PARAMETER & VARIABEL KONTROL ---
float batasSuhu = 30.0; // Batas suhu panas (derajat Celcius)
int batasCahaya = 2000; // Ambang batas ADC LDR (0 - 4095)
unsigned long durasiTanpaGerak =
    10000; // 10 detik tanpa gerakan dianggap ruangan kosong (untuk simulasi)
unsigned long intervalPublish = 3000; // Interval kirim data ke MQTT (3 detik)

bool isPublishingEnabled = true; // Status aktif/nonaktif pengiriman data
bool buzzerMuted = false;        // Status mute buzzer dari dashboard

unsigned long lastPublishTime = 0;
unsigned long lastMotionTime = 0;

String statusRuangan = "normal";
String pesanPeringatan = "normal";

// --- FUNGSI CALLBACK MQTT (Menerima Perintah dari Dashboard) ---
void onMessage(char *topic, byte *payload, unsigned int length) {
  String topik = String(topic);
  String pesan = "";
  for (unsigned int i = 0; i < length; i++) {
    pesan += (char)payload[i];
  }
  pesan.trim();

  Serial.println("Perintah masuk [" + topik + "]: " + pesan);

  // 1. Kontrol Buzzer (contoh: room/control/buzzer = OFF)
  if (topik == "room/control/buzzer") {
    if (pesan.equalsIgnoreCase("OFF")) {
      buzzerMuted = true;
      digitalWrite(BUZZER_PIN, LOW);
      client.publish("room/status", "Konfirmasi: Buzzer dimatikan");
    } else if (pesan.equalsIgnoreCase("ON")) {
      buzzerMuted = false;
      client.publish("room/status", "Konfirmasi: Buzzer diaktifkan");
    }
  }
  // 2. Ubah Batas Suhu (contoh: room/control/batas_suhu = 32)
  else if (topik == "room/control/batas_suhu") {
    float suhuBaru = pesan.toFloat();
    if (suhuBaru > 0) {
      batasSuhu = suhuBaru;
      client.publish("room/status", ("Konfirmasi: Batas suhu diubah ke " +
                                     String(batasSuhu) + " C")
                                        .c_str());
    }
  }
  // 3. Aktif/Nonaktif Pengiriman Data (contoh: room/control/publish = OFF)
  else if (topik == "room/control/publish") {
    if (pesan.equalsIgnoreCase("OFF")) {
      isPublishingEnabled = false;
      client.publish(
          "room/status",
          "Konfirmasi: Pengiriman data dijeda (koneksi tetap aktif)");
    } else if (pesan.equalsIgnoreCase("ON")) {
      isPublishingEnabled = true;
      client.publish("room/status",
                     "Konfirmasi: Pengiriman data diaktifkan kembali");
    }
  }
  // 4. Ubah Interval Pengiriman dalam detik (contoh: room/control/interval = 5)
  else if (topik == "room/control/interval") {
    long detikBaru = pesan.toInt();
    if (detikBaru >= 1) {
      intervalPublish = detikBaru * 1000UL;
      client.publish("room/status", ("Konfirmasi: Interval diubah menjadi " +
                                     String(detikBaru) + " detik")
                                        .c_str());
    }
  }
}

void setupWiFi() {
  delay(10);
  Serial.print("Menghubungkan ke WiFi ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi terhubung! IP: " + WiFi.localIP().toString());
}

void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("Menghubungkan ke MQTT Broker...");
    String clientId = "ESP32-SmartRoom-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("Terhubung!");
      // Subscribe ke semua topik kontrol
      client.subscribe("room/control/#");
      client.publish("room/status", "ESP32 Online & Siap");
    } else {
      Serial.print("Gagal, rc=");
      Serial.print(client.state());
      Serial.println(" coba lagi dalam 3 detik...");
      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  dht.begin();
  setupWiFi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(onMessage);

  lastMotionTime = millis();
}

void loop() {
  // Jaga koneksi MQTT agar komunikasi dua arah selalu aktif
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  unsigned long currentMillis = millis();

  // --- 1. BACA KETIGA SENSOR ---
  float suhu = dht.readTemperature();
  float kelembaban = dht.readHumidity();
  int nilaiLDR = analogRead(LDR_PIN);
  int gerakan = digitalRead(PIR_PIN);

  // Validasi pembacaan DHT11
  if (isnan(suhu) || isnan(kelembaban)) {
    Serial.println("Gagal membaca sensor DHT11!");
    delay(500);
    return;
  }

  // Catat waktu terakhir kali terdeteksi gerakan
  if (gerakan == HIGH) {
    lastMotionTime = currentMillis;
  }

  // Catatan: Pada modul LDR umum, nilai ADC < batasCahaya menandakan ruangan
  // terang. Jika pada simulator slider terang menghasilkan nilai tinggi (>
  // 2000), ubah tanda '<' menjadi '>'.
  bool ruanganTerang = (nilaiLDR < batasCahaya);
  bool tidakAdaOrangLama = (currentMillis - lastMotionTime >= durasiTanpaGerak);

  // --- 2. LOGIKA FUSI SENSOR (SENSOR FUSION) ---
  if (suhu > batasSuhu && gerakan == HIGH) {
    statusRuangan = "panas";
    pesanPeringatan = "ruangan terlalu panas";
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, buzzerMuted ? LOW : HIGH);
  } else if (ruanganTerang && tidakAdaOrangLama) {
    statusRuangan = "boros_energi";
    pesanPeringatan = "pemborosan energi";
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, buzzerMuted ? LOW : HIGH);
  } else {
    statusRuangan = "normal";
    pesanPeringatan = "normal";
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    buzzerMuted =
        false; // Reset status mute otomatis saat kondisi kembali normal
  }

  // --- 3. PUBLISH DATA KE BROKER MQTT SESUAI INTERVAL ---
  if (isPublishingEnabled &&
      (currentMillis - lastPublishTime >= intervalPublish)) {
    lastPublishTime = currentMillis;

    client.publish("room/suhu", String(suhu, 1).c_str());
    client.publish("room/kelembaban", String(kelembaban, 1).c_str());
    client.publish("room/cahaya", String(nilaiLDR).c_str());
    client.publish("room/gerakan", gerakan == HIGH ? "ADA_ORANG" : "KOSONG");
    client.publish("room/alert", pesanPeringatan.c_str());
    client.publish("room/status", statusRuangan.c_str());

    // Tampilkan juga di Serial Monitor untuk pemantauan simulasi
    Serial.println("--- Data Terkirim ke MQTT ---");
    Serial.printf("Suhu: %.1f C | Lembab: %.1f %% | LDR: %d | PIR: %d\n", suhu,
                  kelembaban, nilaiLDR, gerakan);
    Serial.println("Status Fusi: " + pesanPeringatan);
  }
}
