# ComfortRoom

Single-file ESP32/Arduino firmware for a room-comfort monitor (DHT11 temperature/humidity, LDR, PIR, buzzer) that reports over MQTT.

## Build and verify

- Arduino CLI only (no PlatformIO, tests, lint config, or CI). Compiling is the only verification step.
- One-time setup:
  ```sh
  arduino-cli core install esp32:esp32
  arduino-cli lib install "DHT sensor library" "ESP32_WiFi_Connect" "PubSubClient" "ESPAsyncWebServer" "AsyncTCP"
  ```
- Compile from the repo root (the sketch directory name must match `ComfortRoom.ino`):
  ```sh
  arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build .
  ```
- `build/` is generated and gitignored; never edit or commit it. Delete it for a clean rebuild.

## Configuration

- `mqtt_server` / `mqtt_port` near the top of `ComfortRoom.ino` are hardcoded (currently a LAN IP). There is no env or config file.
- WiFi credentials are managed by the `ESP32_WiFi_Connect` library and are not stored here; don't add them to the sketch.
- Pins: PIR `14`, DHT11 `27`, LDR `34`, buzzer `26`.
- All topics derive from `DEVICE_ID` via `TOPIC()`: `comfortroom-<uuid>/room/...`. Changing the ID or `room/` prefix breaks every external consumer.

## MQTT contract

- Publishes retained topics: `status` (`NORMAL|HOT|ENERGY_WASTE`, only when it changes), `temperature`, `humidity`, `light` (raw ADC 0–4095), `motion` (`ON|OFF`), and `availability` (`online`, with LWT `offline`).
- Subscribes to `control/#`; the topic suffix selects the command:
  - `buzzer ON|OFF`, `silence ON|OFF`, `publish ON|OFF`
  - `temp_threshold` 0.1–100, `interval` 1–3600 seconds, `motion_hold` 1–3600 seconds, `light_threshold` 0–4095
- Replies go to `ack` as `command|OK[|value]` or `command|FAILED|E_NUMBER|E_RANGE|E_TOKEN|E_TOPIC|E_UNKNOWN`. Acks queue (max 4) and flush after `client.loop()`.
- Treat topic names and payload formats as a public interface; don't change them without updating consumers.

## Logic gotchas

- The LDR is treated as active-low: `roomIsBright = lightLevel < lightThreshold`. Lower ADC means brighter; don't invert this without checking the wiring.
- `HOT` (temperature above threshold) takes precedence over `ENERGY_WASTE` (bright with no motion for `noMotionHoldMs`).
- `silence` clears automatically when status returns to `NORMAL`; `buzzer OFF` mute persists across states.
- `PubSubClient::publish` has no `String` overload in this setup; pass `.c_str()` for `String` payloads.
- A failed DHT read logs and delays 500 ms, skipping that loop's publish. DHT11 reads are slow; don't poll faster.
- `ESPAsyncWebServer`/`AsyncTCP` are installed per the README but are not currently included; keep them only if adding a web server.
