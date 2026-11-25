# ComfortRoom

Single-file ESP32/Arduino firmware for a room-comfort monitor (DHT11 temperature/humidity, LDR, PIR, buzzer) that reports over MQTT.

## Build and verify

- Arduino CLI only (no PlatformIO, tests, lint config, or CI). Compiling is the only verification step.
- One-time setup:
  ```sh
  arduino-cli core install esp32:esp32
  arduino-cli lib install "DHT sensor library" "ESP32_WiFi_Connect" "PubSubClient" "ESPAsyncWebServer" "AsyncTCP"
  ```
- Compile from the repo root (the sketch directory name must match `ComfortRoom.ino`). `MQTT_SERVER` is the mandatory first-boot broker fallback and must be passed unquoted:
  ```sh
  arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build \
    --build-property compiler.cpp.extra_flags=-DMQTT_SERVER=192.168.1.100 .
  ```
- Don't override `build.extra_flags` for this: it replaces the platform's defaults (dropping `-DESP32`) and library compilation breaks.
- `build/` is generated and gitignored; never edit or commit it. Delete it for a clean rebuild.

## Configuration

- MQTT broker host is runtime-configurable: publish `control/broker <host>` (see contract). It is persisted in NVS (namespace `comfort`, key `mqtt_server`) and the device reconnects after queued acks flush.
- The sketch holds no broker IP. `-DMQTT_SERVER=<host>` supplies the first-boot fallback only; since it is a bare token, hostnames with letters/hyphens/dots work, but it must not be quoted on the command line.
- `mqtt_port` is still hardcoded at `1883`.
- WiFi credentials are managed by the `ESP32_WiFi_Connect` library and are not stored here; don't add them to the sketch.
- Pins: PIR `14`, DHT11 `27`, LDR `34`, buzzer `26`.
- All topics derive from `DEVICE_ID` via `TOPIC()`: `comfortroom-<uuid>/room/...`. Changing the ID or `room/` prefix breaks every external consumer.

## MQTT contract

- Publishes retained topics: `status` (`NORMAL|HOT|ENERGY_WASTE`, only when it changes), `temperature`, `humidity`, `light` (raw ADC 0–4095), `motion` (`ON|OFF`), and `availability` (`online`, with LWT `offline`).
- Subscribes to `control/#`; the topic suffix selects the command:
  - `buzzer ON|OFF`, `silence ON|OFF`, `publish ON|OFF`
  - `temp_threshold` 0.1–100, `interval` 1–3600 seconds, `motion_hold` 1–3600 seconds, `light_threshold` 0–4095
  - `broker` a hostname or IP using only `[A-Za-z0-9._-]`, max 63 chars; saved to NVS and reconnected
- Replies go to `ack` as `command|OK[|value]` or `command|FAILED|E_NUMBER|E_RANGE|E_TOKEN|E_TOPIC|E_NVS|E_UNKNOWN`. Acks queue (max 4) and flush after `client.loop()`.
- Treat topic names and payload formats as a public interface; don't change them without updating consumers.

## Logic gotchas

- The LDR is treated as active-low: `roomIsBright = lightLevel < lightThreshold`. Lower ADC means brighter; don't invert this without checking the wiring.
- `HOT` (temperature above threshold) takes precedence over `ENERGY_WASTE` (bright with no motion for `noMotionHoldMs`).
- `silence` clears automatically when status returns to `NORMAL`; `buzzer OFF` mute persists across states.
- `PubSubClient::publish` has no `String` overload in this setup; pass `.c_str()` for `String` payloads.
- `PubSubClient` keeps the `setServer` pointer, so the host must live in the static `mqttServer` buffer; never pass a temporary `String`.
- `prefs` is already a global in `ESP32_WiFi_Connect`; the sketch's Preferences instance is named `brokerPrefs` to avoid a multiple-definition link error.
- A failed DHT read logs and delays 500 ms, skipping that loop's publish. DHT11 reads are slow; don't poll faster.
- `ESPAsyncWebServer`/`AsyncTCP` are installed per the README but are not currently included; keep them only if adding a web server.
