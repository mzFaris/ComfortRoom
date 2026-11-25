#include <DHT.h>
#include <ESP32_WiFi_Connect.h>
#include <PubSubClient.h>
#include <WiFi.h>

#define PIR_PIN 14
#define DHT_PIN 27
#define LDR_PIN 34
#define BUZZER_PIN 26

#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

#define DEVICE_ID "comfortroom-8f2c41d6-9a3b-4e57-b0d2-6c5a7e19f408"
#define TOPIC_PREFIX DEVICE_ID "/room/"

#define TOPIC(path) (TOPIC_PREFIX path)

// Set this to the LAN IP of the machine running the MQTT broker.
const char *mqtt_server = "192.168.1.100";
const int mqtt_port = 1883;

WiFiClient espClient;
ESP32_WiFi_Connect wifiConnect;

constexpr char clientId[] = "ESP32";

float tempThreshold = 30.0;
int lightThreshold = 2000;
unsigned long noMotionHoldMs = 10000;
unsigned long publishIntervalMs = 3000;

constexpr float TEMP_THRESHOLD_MAX = 100.0;
constexpr int LIGHT_THRESHOLD_MIN = 0;
constexpr int LIGHT_THRESHOLD_MAX = 4095;
constexpr unsigned long NO_MOTION_HOLD_MIN_SEC = 1;
constexpr unsigned long NO_MOTION_HOLD_MAX_SEC = 3600;
constexpr unsigned long PUBLISH_INTERVAL_MIN_SEC = 1;
constexpr unsigned long PUBLISH_INTERVAL_MAX_SEC = 3600;

bool isPublishingEnabled = true;
bool buzzerMuted = false;
bool buzzerSilenced = false;

unsigned long lastPublishMs = 0;
unsigned long lastMotionMs = 0;
unsigned long lastWifiRetryMs = 0;
unsigned long lastMqttAttemptMs = 0;

constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 3000;

enum RoomStatus : uint8_t { STATUS_NORMAL, STATUS_HOT, STATUS_ENERGY_WASTE };

RoomStatus roomStatus = STATUS_NORMAL;

int lastSentStatus = -1;

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

void handleMessage(char *topic, byte *payload, unsigned int length) {
  String incomingTopic = String(topic);
  String text = "";
  for (unsigned int i = 0; i < length; i++) {
    text += (char)payload[i];
  }
  text.trim();

  Serial.println("Incoming command [" + incomingTopic + "]: " + text);

  const char *controlPrefix = TOPIC("control/");
  if (!incomingTopic.startsWith(controlPrefix)) {
    Serial.println("Topic outside " + String(TOPIC("control/#")) + ": " +
                   incomingTopic);
    queueAck("system|FAILED|E_TOPIC");
    return;
  }
  String command = incomingTopic.substring(strlen(controlPrefix));

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

  else if (command == "silence") {
    if (text.equalsIgnoreCase("ON")) {
      buzzerSilenced = true;
      digitalWrite(BUZZER_PIN, LOW);
      queueAck("silence|OK");
    } else if (text.equalsIgnoreCase("OFF")) {
      buzzerSilenced = false;
      queueAck("silence|OK");
    } else {
      queueAck("silence|FAILED|E_TOKEN");
    }
  }

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

  else if (command == "publish") {
    if (text.equalsIgnoreCase("OFF")) {
      isPublishingEnabled = false;
      queueAck("publish|OK");
    } else if (text.equalsIgnoreCase("ON")) {
      isPublishingEnabled = true;

      lastSentStatus = -1;
      queueAck("publish|OK");
    } else {
      queueAck("publish|FAILED|E_TOKEN");
    }
  }

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

  else {
    queueAck(command + "|FAILED|E_UNKNOWN");
  }
}

PubSubClient client(mqtt_server, mqtt_port, handleMessage, espClient);

void logPublishFailure(const char *topic, unsigned int length) {
  Serial.println("Failed to publish to " + String(topic) + " (payload " +
                 String(length) + " bytes)");
}

void flushAckQueue() {
  while (ackQueueCount > 0 && client.connected()) {
    String message = ackQueue[ackQueueTail];
    ackQueueTail = (ackQueueTail + 1) % ACK_QUEUE_SIZE;
    ackQueueCount--;
    client.publish(TOPIC("ack"), message.c_str());
  }
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.print("Connecting to MQTT broker...");

  if (client.connect(clientId, TOPIC("availability"), 1, true, "offline")) {
    Serial.println("connected!");

    if (!client.subscribe(TOPIC("control/#"))) {
      Serial.println("Warning: failed to subscribe to " +
                     String(TOPIC("control/#")));
    }

    client.publish(TOPIC("availability"), "online", true);

    lastSentStatus = -1;
  } else {
    Serial.print("failed, rc=");
    Serial.print(client.state());
    Serial.println(" retrying...");
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();

  wifiConnect.begin();
  Serial.println("WiFi status: " + wifiConnect.getStatus());

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(handleMessage);

  client.setBufferSize(512);

  lastMotionMs = millis();
}

void loop() {

  wifiConnect.keepAlive();

  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {

    if (!client.connected() &&
        now - lastMqttAttemptMs >= MQTT_RETRY_INTERVAL_MS) {
      lastMqttAttemptMs = now;
      connectMQTT();
    }
    if (client.loop()) {
      flushAckQueue();
    }
  } else if (wifiConnect.isConfigured() &&
             now - lastWifiRetryMs >= WIFI_RETRY_INTERVAL_MS) {

    lastWifiRetryMs = now;
    WiFi.reconnect();
  }

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int lightLevel = analogRead(LDR_PIN);
  int motion = digitalRead(PIR_PIN);

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Failed to read DHT11 sensor!");
    delay(500);
    return;
  }

  if (motion == HIGH) {
    lastMotionMs = now;
  }

  bool roomIsBright = (lightLevel < lightThreshold);
  bool someonePresent = (now - lastMotionMs < noMotionHoldMs);

  if (temperature > tempThreshold) {
    roomStatus = STATUS_HOT;
  } else if (roomIsBright && !someonePresent) {
    roomStatus = STATUS_ENERGY_WASTE;
  } else {
    roomStatus = STATUS_NORMAL;
  }

  switch (roomStatus) {
  case STATUS_HOT:
  case STATUS_ENERGY_WASTE:
    digitalWrite(BUZZER_PIN, (buzzerMuted || buzzerSilenced) ? LOW : HIGH);
    break;
  case STATUS_NORMAL:
  default:
    digitalWrite(BUZZER_PIN, LOW);

    buzzerSilenced = false;
    break;
  }

  if (client.connected() && isPublishingEnabled) {
    if ((int)roomStatus != lastSentStatus &&
        client.publish(TOPIC("status"), statusCode(roomStatus), true)) {
      lastSentStatus = (int)roomStatus;
    }

    if (now - lastPublishMs >= publishIntervalMs) {
      lastPublishMs = now;

      client.publish(TOPIC("temperature"), String(temperature, 1).c_str(), true);
      client.publish(TOPIC("humidity"), String(humidity, 1).c_str(), true);
      client.publish(TOPIC("light"), String(lightLevel).c_str(), true);
      client.publish(TOPIC("motion"), someonePresent ? "ON" : "OFF", true);

      Serial.println("--- Data published to MQTT ---");
      Serial.printf("Temp: %.1f C | Humidity: %.1f %% | LDR: %d | PIR: %d\n",
                    temperature, humidity, lightLevel, motion);
      Serial.printf("Fusion status: %s (%s)\n", statusCode(roomStatus),
                    statusDescription(roomStatus));
    }
  }
}
