# Prerequisite
arduino-cli core install esp32:esp32
arduino-cli lib install "DHT sensor library" "ESP32_WiFi_Connect" "PubSubClient" "ESPAsyncWebServer" "AsyncTCP"

# compile
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build .

