# Prerequisite
arduino-cli core install esp32:esp32
arduino-cli lib install "DHT sensor library" "ESP32_WiFi_Connect" "PubSubClient" "ESPAsyncWebServer" "AsyncTCP"

# compile
# MQTT_SERVER is only the first-boot fallback; the broker can be changed later
# over MQTT without reflashing.
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build \
  --build-property compiler.cpp.extra_flags=-DMQTT_SERVER=192.168.1.100 .

# change the broker at runtime (persisted in NVS, device reconnects)
mosquitto_pub -h 192.168.1.100 -t comfort/room/control/broker -m 192.168.1.50
mosquitto_sub -h 192.168.1.100 -t comfort/room/ack -C 1   # prints broker|OK|192.168.1.50
