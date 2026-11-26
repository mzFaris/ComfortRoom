# ComfortRoom

Single-file ESP32/Arduino firmware (`ComfortRoom.ino`): DHT11 temperature/humidity, LDR, PIR, and buzzer, reporting over MQTT.

## Build and verify

- No tests, lint config, or CI. Compiling is the only verification step.
- One-time setup. `ESP32_WiFi_Connect` includes `AsyncTCP.h`/`ESPAsyncWebServer.h`, so both are required even though the sketch doesn't use them directly:
  ```sh
  arduino-cli core install esp32:esp32
  arduino-cli lib install "DHT sensor library" "ESP32_WiFi_Connect" "PubSubClient" \
    "ESP Async WebServer" "Async TCP"
  ```
- Install the `ESP32Async` forks named `"ESP Async WebServer"` and `"Async TCP"`, not the similarly named `"ESPAsyncWebServer"`/`"AsyncTCP"` packages: the latter (3.1.0/1.1.4) fail on esp32 core 3.x with `mbedtls_md5_*_ret` errors. If those old packages are already installed, uninstall them first (`arduino-cli lib uninstall "ESPAsyncWebServer" "AsyncTCP"`).
- Verified with esp32 core 3.3.12, ESP Async WebServer 3.12.1, Async TCP 3.6.1; the sketch already uses ~78% of the default 1.3 MB flash partition, so watch size when adding libraries.
- Compile from the repo root (the directory name must match `ComfortRoom.ino`):
  ```sh
  arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build .
  ```
- The broker host is hardcoded via `MQTT_SERVER` near the top of `ComfortRoom.ino` (default `192.168.1.100`); edit that literal instead of passing build flags. A runtime `control/broker` change persists in NVS and overrides it.
- `build/` is generated and gitignored; never edit it, and delete it for a clean rebuild.

## Runtime configuration

- The hardcoded broker host is the first-boot fallback. A runtime `control/broker <host>` change is persisted in NVS (namespace `comfort`, key `mqtt_server`) and overrides it; the device acks on the old broker, then reconnects. `mqtt_port` is hardcoded to `1883`.
- Only the broker host persists. Thresholds, intervals, publish state, and buzzer mute/silence all reset to defaults on reboot.
- WiFi credentials are managed by `ESP32_WiFi_Connect`, not this sketch; don't add them here.
- Pins: PIR `14`, DHT11 `27`, LDR `34`, buzzer `26`.

## MQTT interface (treat as public contract)

- All topics derive from `DEVICE_ID` via `TOPIC()`: currently `comfortroom/room/...`. Changing `DEVICE_ID` or the `room/` prefix breaks external consumers.
- Publishes retained `status` (`NORMAL|HOT|ENERGY_WASTE`, only when it changes), `temperature`, `humidity`, `light` (raw ADC 0–4095), `motion` (`ON|OFF`), and `availability` (`online`, LWT `offline`).
- Subscribes to `control/#`; the topic suffix selects the command: `buzzer`, `silence`, `publish` (`ON|OFF`); `temp_threshold` (0, 100]; `interval` and `motion_hold` 1–3600 seconds; `light_threshold` 0–4095; `broker` host.
- Acks go to `ack` as `command|OK[|value]` or `command|FAILED|E_NUMBER|E_RANGE|E_TOKEN|E_TOPIC|E_NVS|E_UNKNOWN`. The queue holds 4 and flushes after `client.loop()`.
- `clientId` is hardcoded to `ESP32` and `DEVICE_ID` to `comfortroom`, so two devices on one broker will disconnect each other and share topics.

## Logic and toolchain gotchas

- The LDR is treated as active-low: `roomIsBright = lightLevel < lightThreshold` (lower ADC means brighter). Don't invert without checking the wiring.
- `HOT` takes precedence over `ENERGY_WASTE` (bright with no motion for `noMotionHoldMs`).
- `silence` clears automatically when status returns to `NORMAL`; `buzzer OFF` mute persists across states.
- `PubSubClient::publish` has no `String` overload here; pass `.c_str()`. It also stores the `setServer` pointer, so the host must stay in the static `mqttServer` buffer — never pass a temporary `String`.
- `prefs` is a global in `ESP32_WiFi_Connect`; the sketch's Preferences instance is named `brokerPrefs` to avoid a multiple-definition link error.
- A failed DHT read logs, delays 500 ms, and skips that loop's publish. DHT11 reads are slow; don't poll faster.
- The broker-change sequence is order-sensitive: flush queued acks on the old broker before `disconnect()`/`setServer()` (see `brokerChangePending` in `loop()`).
