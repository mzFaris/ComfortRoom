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

// --- IDENTITAS PERANGKAT (namespace topik MQTT) ---
// Semua topik memakai awalan unik per perangkat: comfortroom-<uuid>/room/...
// Broker publik (broker.hivemq.com) dipakai bersama, jadi awalan ini mencegah
// perangkat lain membaca data kita atau mengirim perintah ke kita.
// Ganti UUID di bawah untuk setiap unit fisik yang berbeda.
#define ID_PERANGKAT "comfortroom-8f2c41d6-9a3b-4e57-b0d2-6c5a7e19f408"
#define AWALAN_TOPIK ID_PERANGKAT "/room/"

// Topik dibentuk saat kompilasi (penggabungan literal), bukan saat runtime,
// sehingga tidak ada alokasi heap: TOPIK("suhu") ->
// "comfortroom-8f2c.../room/suhu"
#define TOPIK(jalur) (AWALAN_TOPIK jalur)

// --- KONFIGURASI WIFI & MQTT ---
// Placeholder: isi dengan kredensial WiFi Anda sebelum mengunggah.
// Untuk simulasi Wokwi gunakan ssid "Wokwi-GUEST" dan password "".
const char *ssid = "NAMA_WIFI_ANDA";
const char *password = "PASSWORD_WIFI_ANDA";
const char *mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;

// --- PARAMETER & VARIABEL KONTROL ---
float batasSuhu = 30.0; // Batas suhu panas (derajat Celcius)
int batasCahaya = 2000; // Ambang batas ADC LDR (0 - 4095)
unsigned long durasiTanpaGerak =
    10000; // 10 detik tanpa gerakan dianggap ruangan kosong (untuk simulasi)
unsigned long intervalPublish = 3000; // Interval kirim data ke MQTT (3 detik)

// Batas nilai yang diterima dari dashboard (validasi perintah)
const float BATAS_SUHU_MAKS = 100.0;          // derajat Celcius
const int BATAS_CAHAYA_MIN = 0;               // batas bawah ADC
const int BATAS_CAHAYA_MAKS = 4095;           // batas atas ADC
const unsigned long DURASI_GERAK_MINS = 1;    // detik
const unsigned long DURASI_GERAK_MAKS = 3600; // detik
const unsigned long INTERVAL_MIN = 1;         // detik
const unsigned long INTERVAL_MAKS = 3600;     // detik

bool isPublishingEnabled = true; // Status aktif/nonaktif pengiriman data
bool buzzerMuted = false;        // Status mute buzzer dari dashboard

unsigned long lastPublishTime = 0;
unsigned long lastMotionTime = 0;

// --- STATUS RUANGAN (hasil fusi sensor) ---
// Disimpan sebagai enum agar hanya tiga kondisi yang sah. Yang dikirim ke MQTT
// hanyalah KODE (namaStatus); kalimat yang dibaca pengguna diatur dashboard.
enum StatusRuangan : uint8_t { NORMAL, PANAS, BOROS_ENERGI };

StatusRuangan statusRuangan = NORMAL;

// Status terakhir yang sudah terkirim (-1 = belum pernah / paksa kirim).
int statusTerakhirDikirim = -1;

// Kode status untuk topik MQTT. Jangan diubah tanpa memperbarui dashboard.
const char *namaStatus(StatusRuangan status) {
  switch (status) {
  case PANAS:
    return "PANAS";
  case BOROS_ENERGI:
    return "BOROS";
  case NORMAL:
  default:
    return "NORMAL";
  }
}

// Teks penjelasan hanya untuk Serial Monitor (lokal), tidak dikirim ke broker.
const char *pesanStatus(StatusRuangan status) {
  switch (status) {
  case PANAS:
    return "ruangan terlalu panas";
  case BOROS_ENERGI:
    return "pemborosan energi";
  case NORMAL:
  default:
    return "normal";
  }
}

// --- ANTREAN ACK (Konfirmasi Perintah dari Dashboard) ---
// Konfirmasi tidak dikirim langsung dari callback, melainkan diantrekan lalu
// dikirim di loop() setelah client.loop() selesai. Ini menghindari publish saat
// buffer MQTT sedang dipakai untuk menerima pesan masuk.
const int KAPASITAS_ANTREAN_ACK = 4;
String antreanAck[KAPASITAS_ANTREAN_ACK];
int indeksMasukAck = 0;
int indeksKeluarAck = 0;
int jumlahAck = 0;

void jadwalkanAck(const String &pesan) {
  if (jumlahAck >= KAPASITAS_ANTREAN_ACK) {
    Serial.println("Antrean ack penuh, pesan dibuang: " + pesan);
    return;
  }
  antreanAck[indeksMasukAck] = pesan;
  indeksMasukAck = (indeksMasukAck + 1) % KAPASITAS_ANTREAN_ACK;
  jumlahAck++;
}

// --- PEMBACA ANGKA PAYLOAD (validasi perintah) ---
// Mengembalikan false bila payload kosong atau mengandung karakter non-angka,
// sehingga "abc" tidak lagi diam-diam dianggap 0.
bool parseAngka(const String &pesan, long &hasil) {
  if (pesan.length() == 0) {
    return false;
  }
  char *akhir = nullptr;
  hasil = strtol(pesan.c_str(), &akhir, 10);
  return akhir != nullptr && *akhir == '\0';
}

bool parseDesimal(const String &pesan, float &hasil) {
  if (pesan.length() == 0) {
    return false;
  }
  char *akhir = nullptr;
  hasil = strtof(pesan.c_str(), &akhir);
  return akhir != nullptr && *akhir == '\0';
}

// --- FUNGSI CALLBACK MQTT (Menerima Perintah dari Dashboard) ---
// Format ack (tidak berisi kalimat; teks untuk pengguna diatur dashboard):
//   <perintah>|OK|<nilai>     contoh: batas_suhu|OK|32.0
//   <perintah>|GAGAL|<kode>   contoh: interval|GAGAL|E_RANGE
// Kode kesalahan: E_TOKEN (bukan ON/OFF), E_NUMBER (bukan angka),
// E_RANGE (di luar rentang), E_UNKNOWN (perintah tak dikenal),
// E_TOPIC (topik di luar control/#).
void onMessage(char *topic, byte *payload, unsigned int length) {
  String topikMasuk = String(topic);
  String pesan = "";
  for (unsigned int i = 0; i < length; i++) {
    pesan += (char)payload[i];
  }
  pesan.trim();

  Serial.println("Perintah masuk [" + topikMasuk + "]: " + pesan);

  // Ambil nama perintah relatif terhadap "<id>/room/control/"
  const char *awalanPerintah = TOPIK("control/");
  if (!topikMasuk.startsWith(awalanPerintah)) {
    Serial.println("Topik di luar " + String(TOPIK("control/#")) + ": " +
                   topikMasuk);
    jadwalkanAck("sistem|GAGAL|E_TOPIC");
    return;
  }
  String perintah = topikMasuk.substring(strlen(awalanPerintah));

  // 1. Kontrol Buzzer (contoh: control/buzzer = OFF)
  if (perintah == "buzzer") {
    if (pesan.equalsIgnoreCase("OFF")) {
      buzzerMuted = true;
      digitalWrite(BUZZER_PIN, LOW);
      jadwalkanAck("buzzer|OK");
    } else if (pesan.equalsIgnoreCase("ON")) {
      buzzerMuted = false;
      jadwalkanAck("buzzer|OK");
    } else {
      jadwalkanAck("buzzer|GAGAL|E_TOKEN");
    }
  }
  // 2. Ubah Batas Suhu (contoh: control/batas_suhu = 32)
  else if (perintah == "batas_suhu") {
    float suhuBaru = 0;
    if (!parseDesimal(pesan, suhuBaru)) {
      jadwalkanAck("batas_suhu|GAGAL|E_NUMBER");
    } else if (suhuBaru <= 0 || suhuBaru > BATAS_SUHU_MAKS) {
      jadwalkanAck("batas_suhu|GAGAL|E_RANGE");
    } else {
      batasSuhu = suhuBaru;
      jadwalkanAck(String("batas_suhu|OK|") + String(batasSuhu, 1));
    }
  }
  // 3. Aktif/Nonaktif Pengiriman Data (contoh: control/publish = OFF)
  else if (perintah == "publish") {
    if (pesan.equalsIgnoreCase("OFF")) {
      isPublishingEnabled = false;
      jadwalkanAck("publish|OK");
    } else if (pesan.equalsIgnoreCase("ON")) {
      isPublishingEnabled = true;
      // Paksa status dikirim ulang setelah pengiriman diaktifkan kembali
      statusTerakhirDikirim = -1;
      jadwalkanAck("publish|OK");
    } else {
      jadwalkanAck("publish|GAGAL|E_TOKEN");
    }
  }
  // 4. Ubah Interval Pengiriman dalam detik (contoh: control/interval = 5)
  else if (perintah == "interval") {
    long detikBaru = 0;
    if (!parseAngka(pesan, detikBaru)) {
      jadwalkanAck("interval|GAGAL|E_NUMBER");
    } else if (detikBaru < (long)INTERVAL_MIN ||
               detikBaru > (long)INTERVAL_MAKS) {
      jadwalkanAck("interval|GAGAL|E_RANGE");
    } else {
      intervalPublish = detikBaru * 1000UL;
      jadwalkanAck(String("interval|OK|") + String(detikBaru));
    }
  }
  // 5. Ubah Durasi Tanpa Gerak dalam detik (contoh: control/durasi_gerak = 15)
  else if (perintah == "durasi_gerak") {
    long detikBaru = 0;
    if (!parseAngka(pesan, detikBaru)) {
      jadwalkanAck("durasi_gerak|GAGAL|E_NUMBER");
    } else if (detikBaru < (long)DURASI_GERAK_MINS ||
               detikBaru > (long)DURASI_GERAK_MAKS) {
      jadwalkanAck("durasi_gerak|GAGAL|E_RANGE");
    } else {
      durasiTanpaGerak = detikBaru * 1000UL;
      jadwalkanAck(String("durasi_gerak|OK|") + String(detikBaru));
    }
  }
  // 6. Ubah Ambang Cahaya LDR (contoh: control/batas_cahaya = 2500)
  else if (perintah == "batas_cahaya") {
    long nilaiBaru = 0;
    if (!parseAngka(pesan, nilaiBaru)) {
      jadwalkanAck("batas_cahaya|GAGAL|E_NUMBER");
    } else if (nilaiBaru < BATAS_CAHAYA_MIN || nilaiBaru > BATAS_CAHAYA_MAKS) {
      jadwalkanAck("batas_cahaya|GAGAL|E_RANGE");
    } else {
      batasCahaya = (int)nilaiBaru;
      jadwalkanAck(String("batas_cahaya|OK|") + String(batasCahaya));
    }
  }
  // 7. Perintah tidak dikenal: jangan diam, laporkan ke dashboard
  else {
    jadwalkanAck(perintah + "|GAGAL|E_UNKNOWN");
  }
}

PubSubClient client(mqtt_server, mqtt_port, onMessage, espClient);

// --- HELPER PUBLISH DENGAN ERROR HANDLING ---
// Mengembalikan true bila broker menerima paket. Kegagalan biasanya berarti
// buffer MQTT penuh (topik + payload terlalu panjang).
void logGagalPublish(const char *topikTujuan, unsigned int panjang) {
  Serial.println("Gagal publish ke " + String(topikTujuan) + " (payload " +
                 String(panjang) + " byte)");
}

bool kirim(const char *topikTujuan, const char *payload,
           bool retain = false) {
  if (!client.publish(topikTujuan, payload, retain)) {
    logGagalPublish(topikTujuan, strlen(payload));
    return false;
  }
  return true;
}

// Overload untuk payload yang perlu diformat dulu (angka, teks gabungan)
bool kirim(const char *topikTujuan, const String &payload,
           bool retain = false) {
  if (!client.publish(topikTujuan, payload.c_str(), retain)) {
    logGagalPublish(topikTujuan, payload.length());
    return false;
  }
  return true;
}

void kirimAckTertunda() {
  while (jumlahAck > 0 && client.connected()) {
    String pesan = antreanAck[indeksKeluarAck];
    indeksKeluarAck = (indeksKeluarAck + 1) % KAPASITAS_ANTREAN_ACK;
    jumlahAck--;
    kirim(TOPIK("ack"), pesan);
  }
}

void setupWiFi() {
  Serial.print("Menghubungkan ke WiFi ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  // Dibatasi 20 detik agar sketch tidak menggantung saat placeholder belum
  // diisi; percobaan ulang ditangani reconnectMQTT() di loop().
  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi terhubung! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println(
        "\nWiFi gagal tersambung. Periksa ssid/password (masih placeholder?)");
  }
}

void reconnectMQTT() {
  while (!client.connected()) {
    // WiFi bisa putus lebih dulu daripada MQTT: sambungkan ulang dulu
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi terputus, menyambung ulang...");
      WiFi.reconnect();
      unsigned long mulai = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - mulai < 10000) {
        delay(500);
        Serial.print(".");
      }
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi belum tersambung, ulangi dalam 3 detik...");
        delay(3000);
        continue;
      }
    }

    Serial.print("Menghubungkan ke MQTT Broker...");
    String clientId = "ESP32-SmartRoom-" + String(random(0xffff), HEX);

    // Last Will and Testament: broker mengumumkan "offline" (retained) bila
    // perangkat mati mendadak, sehingga dashboard tidak menampilkan data basi.
    // "online" di topik ketersediaan sudah cukup sebagai tanda perangkat siap,
    // jadi tidak perlu ack tambahan.
    if (client.connect(clientId.c_str(), TOPIK("ketersediaan"), 1, true,
                       "offline")) {
      Serial.println("Terhubung!");

      if (!client.subscribe(TOPIK("control/#"))) {
        Serial.println("Peringatan: gagal subscribe ke " +
                       String(TOPIK("control/#")));
      }

      kirim(TOPIK("ketersediaan"), "online", true);

      // Paksa status dikirim ulang setelah reconnect
      statusTerakhirDikirim = -1;
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
  // Topik berawalan ID perangkat + pesan ack lebih panjang dari default 256
  client.setBufferSize(512);

  lastMotionTime = millis();
}

void loop() {
  // Jaga koneksi MQTT agar komunikasi dua arah selalu aktif
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  // Kirim konfirmasi perintah di luar callback
  kirimAckTertunda();

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
  bool adaOrang = (currentMillis - lastMotionTime < durasiTanpaGerak);

  // --- 2. LOGIKA FUSI SENSOR (SENSOR FUSION) ---
  if (suhu > batasSuhu && gerakan == HIGH) {
    statusRuangan = PANAS;
  } else if (ruanganTerang && !adaOrang) {
    statusRuangan = BOROS_ENERGI;
  } else {
    statusRuangan = NORMAL;
  }

  // --- 3. AKTUATOR MENGIKUTI STATUS (LED & buzzer diatur di satu tempat) ---
  switch (statusRuangan) {
  case PANAS:
  case BOROS_ENERGI:
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, buzzerMuted ? LOW : HIGH);
    break;
  case NORMAL:
  default:
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    buzzerMuted =
        false; // Reset status mute otomatis saat kondisi kembali normal
    break;
  }

  // --- 4. PUBLISH STATUS SAAT BERUBAH ---
  // Hanya kode status yang dikirim; topik alert dihapus karena isinya selalu
  // bisa diturunkan dari status. Dashboard memetakan kode -> teks yang bisa
  // diubah pengguna. retained=true agar dashboard yang baru terhubung langsung
  // menerima kondisi terakhir.
  if (isPublishingEnabled && (int)statusRuangan != statusTerakhirDikirim) {
    if (kirim(TOPIK("status"), namaStatus(statusRuangan), true)) {
      statusTerakhirDikirim = (int)statusRuangan;
    }
  }

  // --- 5. PUBLISH DATA SENSOR KE BROKER MQTT SESUAI INTERVAL ---
  if (isPublishingEnabled &&
      (currentMillis - lastPublishTime >= intervalPublish)) {
    lastPublishTime = currentMillis;

    kirim(TOPIK("suhu"), String(suhu, 1), true);
    kirim(TOPIK("kelembaban"), String(kelembaban, 1), true);
    kirim(TOPIK("cahaya"), String(nilaiLDR), true);
    kirim(TOPIK("gerakan"), adaOrang ? "ON" : "OFF", true);

    // Tampilkan juga di Serial Monitor untuk pemantauan simulasi
    Serial.println("--- Data Terkirim ke MQTT ---");
    Serial.printf("Suhu: %.1f C | Lembab: %.1f %% | LDR: %d | PIR: %d\n", suhu,
                  kelembaban, nilaiLDR, gerakan);
    Serial.printf("Status Fusi: %s (%s)\n", namaStatus(statusRuangan),
                  pesanStatus(statusRuangan));
  }
}
