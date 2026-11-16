#include <DHT.h>
#include <PubSubClient.h>
#include <WiFi.h>

// --- PIN CONFIGURATION (matches the simulated circuit) ---
#define PIR_PIN 14    // D14: PIR sensor input
#define DHT_PIN 27    // D27: DHT11 sensor input
#define LDR_PIN 34    // D34 (ADC1): LDR analog input
#define BUZZER_PIN 26 // D26: buzzer output
#define LED_PIN 4     // D4: external indicator LED

#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// --- DEVICE IDENTITY (MQTT topic namespace) ---
// Every topic is prefixed with a per-device namespace: comfortroom-<uuid>/room/...
// The broker (broker.hivemq.com) is public and shared, so this prefix is what
// keeps other devices from reading our data or sending us commands.
// Use a different UUID for each physical unit.
#define DEVICE_ID "comfortroom-8f2c41d6-9a3b-4e57-b0d2-6c5a7e19f408"
#define TOPIC_PREFIX DEVICE_ID "/room/"

// Topics are concatenated at compile time (adjacent string literals) instead of
// at runtime, so building one performs no heap allocation:
// TOPIC("temperature") -> "comfortroom-8f2c.../room/temperature"
#define TOPIC(path) (TOPIC_PREFIX path)

// --- WIFI & MQTT CONFIGURATION ---
// Placeholders: fill in your own WiFi credentials before uploading.
// For the Wokwi simulator use ssid "Wokwi-GUEST" and password "".
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";
const char *mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;

// --- CONTROL PARAMETERS & STATE ---
float tempThreshold = 30.0;  // Temperature that counts as hot (degrees Celsius)
int lightThreshold = 2000;   // LDR ADC threshold (0 - 4095)
unsigned long noMotionHoldMs =
    10000; // 10 s without motion means the room is empty (simulation value)
unsigned long publishIntervalMs = 3000; // Telemetry interval (3 s)

// Accepted value ranges for dashboard commands (command validation)
const float TEMP_THRESHOLD_MAX = 100.0;            // degrees Celsius
const int LIGHT_THRESHOLD_MIN = 0;                 // ADC lower bound
const int LIGHT_THRESHOLD_MAX = 4095;              // ADC upper bound
const unsigned long NO_MOTION_HOLD_MIN_SEC = 1;    // seconds
const unsigned long NO_MOTION_HOLD_MAX_SEC = 3600; // seconds
const unsigned long PUBLISH_INTERVAL_MIN_SEC = 1;  // seconds
const unsigned long PUBLISH_INTERVAL_MAX_SEC = 3600; // seconds

bool isPublishingEnabled = true; // Whether telemetry publishing is active
bool buzzerMuted = false;        // Buzzer muted from the dashboard

unsigned long lastPublishMs = 0;
unsigned long lastMotionMs = 0;

// --- ROOM STATUS (sensor fusion result) ---
// Kept as an enum so only three conditions can exist. Only the CODE produced by
// statusCode() goes over MQTT; the wording shown to users belongs to the
// dashboard.
enum RoomStatus : uint8_t { STATUS_NORMAL, STATUS_HOT, STATUS_ENERGY_WASTE };

RoomStatus roomStatus = STATUS_NORMAL;

// Last status that was published (-1 = never sent / force a resend).
int lastSentStatus = -1;

// Status code published to MQTT. Do not change without updating the dashboard.
const char *statusCode(RoomStatus status) {
  switch (status) {
  case STATUS_HOT:
    return "HOT";
  case STATUS_ENERGY_WASTE:
    return "ENERGY_WASTE";
  case STATUS_NORMAL:
  default:
    return "NORMAL";
  }
}

// Description used by the Serial Monitor only; never published to the broker.
const char *statusDescription(RoomStatus status) {
  switch (status) {
  case STATUS_HOT:
    return "room too hot";
  case STATUS_ENERGY_WASTE:
    return "energy waste";
  case STATUS_NORMAL:
  default:
    return "normal";
  }
}

// --- ACK QUEUE (command confirmations for the dashboard) ---
// Confirmations are not published straight from the callback; they are queued
// and flushed from loop() after client.loop() returns. This avoids publishing
// while the MQTT buffer is still in use by the incoming message.
const int ACK_QUEUE_SIZE = 4;
String ackQueue[ACK_QUEUE_SIZE];
int ackQueueHead = 0;
int ackQueueTail = 0;
int ackQueueCount = 0;

void queueAck(const String &message) {
  if (ackQueueCount >= ACK_QUEUE_SIZE) {
    Serial.println("Ack queue full, dropping message: " + message);
    return;
  }
  ackQueue[ackQueueHead] = message;
  ackQueueHead = (ackQueueHead + 1) % ACK_QUEUE_SIZE;
  ackQueueCount++;
}

// --- PAYLOAD NUMBER PARSERS (command validation) ---
// Return false when the payload is empty or contains non-numeric characters, so
// "abc" is no longer silently treated as 0.
bool parseInteger(const String &text, long &value) {
  if (text.length() == 0) {
    return false;
  }
  char *end = nullptr;
  value = strtol(text.c_str(), &end, 10);
  return end != nullptr && *end == '\0';
}

bool parseDecimal(const String &text, float &value) {
  if (text.length() == 0) {
    return false;
  }
  char *end = nullptr;
  value = strtof(text.c_str(), &end);
  return end != nullptr && *end == '\0';
}

// --- MQTT CALLBACK (commands coming from the dashboard) ---
// Ack formats (no prose; user-facing wording is owned by the dashboard):
//   <command>|OK|<value>      example: temp_threshold|OK|32.0
//   <command>|FAILED|<code>   example: interval|FAILED|E_RANGE
// Error codes: E_TOKEN (not ON/OFF), E_NUMBER (not numeric),
// E_RANGE (outside bounds), E_UNKNOWN (unknown command),
// E_TOPIC (topic outside control/#).
void handleMessage(char *topic, byte *payload, unsigned int length) {
  String incomingTopic = String(topic);
  String text = "";
  for (unsigned int i = 0; i < length; i++) {
    text += (char)payload[i];
  }
  text.trim();

  Serial.println("Incoming command [" + incomingTopic + "]: " + text);

  // Take the command name relative to "<id>/room/control/"
  const char *controlPrefix = TOPIC("control/");
  if (!incomingTopic.startsWith(controlPrefix)) {
    Serial.println("Topic outside " + String(TOPIC("control/#")) + ": " +
                   incomingTopic);
    queueAck("system|FAILED|E_TOPIC");
    return;
  }
  String command = incomingTopic.substring(strlen(controlPrefix));

  // 1. Buzzer control (e.g. control/buzzer = OFF)
  if (command == "buzzer") {
    if (text.equalsIgnoreCase("OFF")) {
      buzzerMuted = true;
      digitalWrite(BUZZER_PIN, LOW);
      queueAck("buzzer|OK");
    } else if (text.equalsIgnoreCase("ON")) {
      buzzerMuted = false;
      queueAck("buzzer|OK");
    } else {
      queueAck("buzzer|FAILED|E_TOKEN");
    }
  }
  // 2. Change the temperature threshold (e.g. control/temp_threshold = 32)
  else if (command == "temp_threshold") {
    float newTemperature = 0;
    if (!parseDecimal(text, newTemperature)) {
      queueAck("temp_threshold|FAILED|E_NUMBER");
    } else if (newTemperature <= 0 || newTemperature > TEMP_THRESHOLD_MAX) {
      queueAck("temp_threshold|FAILED|E_RANGE");
    } else {
      tempThreshold = newTemperature;
      queueAck(String("temp_threshold|OK|") + String(tempThreshold, 1));
    }
  }
  // 3. Enable/disable telemetry publishing (e.g. control/publish = OFF)
  else if (command == "publish") {
    if (text.equalsIgnoreCase("OFF")) {
      isPublishingEnabled = false;
      queueAck("publish|OK");
    } else if (text.equalsIgnoreCase("ON")) {
      isPublishingEnabled = true;
      // Force a status resend once publishing is enabled again
      lastSentStatus = -1;
      queueAck("publish|OK");
    } else {
      queueAck("publish|FAILED|E_TOKEN");
    }
  }
  // 4. Change the publish interval in seconds (e.g. control/interval = 5)
  else if (command == "interval") {
    long newSeconds = 0;
    if (!parseInteger(text, newSeconds)) {
      queueAck("interval|FAILED|E_NUMBER");
    } else if (newSeconds < (long)PUBLISH_INTERVAL_MIN_SEC ||
               newSeconds > (long)PUBLISH_INTERVAL_MAX_SEC) {
      queueAck("interval|FAILED|E_RANGE");
    } else {
      publishIntervalMs = newSeconds * 1000UL;
      queueAck(String("interval|OK|") + String(newSeconds));
    }
  }
  // 5. Change the no-motion hold in seconds (e.g. control/motion_hold = 15)
  else if (command == "motion_hold") {
    long newSeconds = 0;
    if (!parseInteger(text, newSeconds)) {
      queueAck("motion_hold|FAILED|E_NUMBER");
    } else if (newSeconds < (long)NO_MOTION_HOLD_MIN_SEC ||
               newSeconds > (long)NO_MOTION_HOLD_MAX_SEC) {
      queueAck("motion_hold|FAILED|E_RANGE");
    } else {
      noMotionHoldMs = newSeconds * 1000UL;
      queueAck(String("motion_hold|OK|") + String(newSeconds));
    }
  }
  // 6. Change the LDR light threshold (e.g. control/light_threshold = 2500)
  else if (command == "light_threshold") {
    long newValue = 0;
    if (!parseInteger(text, newValue)) {
      queueAck("light_threshold|FAILED|E_NUMBER");
    } else if (newValue < LIGHT_THRESHOLD_MIN ||
               newValue > LIGHT_THRESHOLD_MAX) {
      queueAck("light_threshold|FAILED|E_RANGE");
    } else {
      lightThreshold = (int)newValue;
      queueAck(String("light_threshold|OK|") + String(lightThreshold));
    }
  }
  // 7. Unknown command: report it instead of staying silent
  else {
    queueAck(command + "|FAILED|E_UNKNOWN");
  }
}

PubSubClient client(mqtt_server, mqtt_port, handleMessage, espClient);

// --- PUBLISH HELPER WITH ERROR HANDLING ---
// Returns true when the broker accepted the packet. A failure usually means the
// MQTT buffer is full (topic plus payload too long).
void logPublishFailure(const char *topic, unsigned int length) {
  Serial.println("Failed to publish to " + String(topic) + " (payload " +
                 String(length) + " bytes)");
}

bool publishMessage(const char *topic, const char *payload,
                    bool retain = false) {
  if (!client.publish(topic, payload, retain)) {
    logPublishFailure(topic, strlen(payload));
    return false;
  }
  return true;
}

// Overload for payloads that must be formatted first (numbers, joined text)
bool publishMessage(const char *topic, const String &payload,
                    bool retain = false) {
  if (!client.publish(topic, payload.c_str(), retain)) {
    logPublishFailure(topic, payload.length());
    return false;
  }
  return true;
}

void flushAckQueue() {
  while (ackQueueCount > 0 && client.connected()) {
    String message = ackQueue[ackQueueTail];
    ackQueueTail = (ackQueueTail + 1) % ACK_QUEUE_SIZE;
    ackQueueCount--;
    publishMessage(TOPIC("ack"), message);
  }
}

void connectWiFi() {
  Serial.print("Connecting to WiFi ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  // Bounded to 20 s so the sketch cannot hang while the placeholders are still
  // unfilled; retries are handled by connectMQTT() in loop().
  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println(
        "\nWiFi connection failed. Check ssid/password (still placeholders?)");
  }
}

void connectMQTT() {
  while (!client.connected()) {
    // WiFi can drop before MQTT does, so reconnect it first
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      WiFi.reconnect();
      unsigned long startedAt = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 10000) {
        delay(500);
        Serial.print(".");
      }
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi still down, retrying in 3 s...");
        delay(3000);
        continue;
      }
    }

    Serial.print("Connecting to MQTT broker...");
    String clientId = "ESP32-SmartRoom-" + String(random(0xffff), HEX);

    // Last will and testament: the broker announces "offline" (retained) if the
    // device dies unexpectedly, so the dashboard never shows stale data.
    // "online" on the availability topic already signals readiness, so no extra
    // ack is needed here.
    if (client.connect(clientId.c_str(), TOPIC("availability"), 1, true,
                       "offline")) {
      Serial.println("connected!");

      if (!client.subscribe(TOPIC("control/#"))) {
        Serial.println("Warning: failed to subscribe to " +
                       String(TOPIC("control/#")));
      }

      publishMessage(TOPIC("availability"), "online", true);

      // Force a status resend after reconnecting
      lastSentStatus = -1;
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 3 s...");
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
  connectWiFi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(handleMessage);
  // Prefixed topics plus ack messages exceed the default 256-byte buffer
  client.setBufferSize(512);

  lastMotionMs = millis();
}

void loop() {
  // Keep the MQTT connection alive so two-way communication keeps working
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();

  // Send queued command confirmations outside the callback
  flushAckQueue();

  unsigned long now = millis();

  // --- 1. READ ALL THREE SENSORS ---
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int lightLevel = analogRead(LDR_PIN);
  int motion = digitalRead(PIR_PIN);

  // Validate the DHT11 readings
  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Failed to read DHT11 sensor!");
    delay(500);
    return;
  }

  // Record the last time motion was detected
  if (motion == HIGH) {
    lastMotionMs = now;
  }

  // Note: on common LDR modules an ADC reading below lightThreshold means the
  // room is bright. If the simulator's brightness slider produces a high value
  // (> 2000), invert the comparison below.
  bool roomIsBright = (lightLevel < lightThreshold);
  bool someonePresent = (now - lastMotionMs < noMotionHoldMs);

  // --- 2. SENSOR FUSION ---
  if (temperature > tempThreshold && motion == HIGH) {
    roomStatus = STATUS_HOT;
  } else if (roomIsBright && !someonePresent) {
    roomStatus = STATUS_ENERGY_WASTE;
  } else {
    roomStatus = STATUS_NORMAL;
  }

  // --- 3. ACTUATORS FOLLOW THE STATUS (LED & buzzer driven in one place) ---
  switch (roomStatus) {
  case STATUS_HOT:
  case STATUS_ENERGY_WASTE:
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, buzzerMuted ? LOW : HIGH);
    break;
  case STATUS_NORMAL:
  default:
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    buzzerMuted = false; // Clear the dashboard mute once the room is normal
    break;
  }

  // --- 4. PUBLISH STATUS ON CHANGE ---
  // Only the status code is sent; the alert topic was removed because its
  // content could always be derived from the status. The dashboard maps code ->
  // text, so wording stays editable there. retained=true so a dashboard that
  // connects later immediately receives the current condition.
  if (isPublishingEnabled && (int)roomStatus != lastSentStatus) {
    if (publishMessage(TOPIC("status"), statusCode(roomStatus), true)) {
      lastSentStatus = (int)roomStatus;
    }
  }

  // --- 5. PUBLISH SENSOR DATA TO THE MQTT BROKER ON INTERVAL ---
  if (isPublishingEnabled && (now - lastPublishMs >= publishIntervalMs)) {
    lastPublishMs = now;

    publishMessage(TOPIC("temperature"), String(temperature, 1), true);
    publishMessage(TOPIC("humidity"), String(humidity, 1), true);
    publishMessage(TOPIC("light"), String(lightLevel), true);
    publishMessage(TOPIC("motion"), someonePresent ? "ON" : "OFF", true);

    // Also show it on the Serial Monitor for simulation monitoring
    Serial.println("--- Data published to MQTT ---");
    Serial.printf("Temp: %.1f C | Humidity: %.1f %% | LDR: %d | PIR: %d\n",
                  temperature, humidity, lightLevel, motion);
    Serial.printf("Fusion status: %s (%s)\n", statusCode(roomStatus),
                  statusDescription(roomStatus));
  }
}
