# AGENTS.md

Two projects, each with its own flake: the ESP32 firmware in `firmware/ComfortRoom/` (single sketch) and a SolidJS + Vite dashboard in `dashboard/`. There is no root flake; run `nix develop ./firmware` or `nix develop ./dashboard` from the repo root. No test suite, linter, or CI; a successful firmware compile and dashboard build are the only automated checks.

## Build, flash, verify

- One-time setup: `arduino-cli core install esp32:esp32`, then `arduino-cli lib install "DHT sensor library" PubSubClient "Adafruit Unified Sensor" ESP32_WiFi_Connect`. Without the sensor/MQTT libraries the compile fails with `fatal error: DHT.h: No such file or directory`. `firmware/flake.nix` provides `arduino-cli` and `mosquitto`, so `nix develop ./firmware -c arduino-cli ...` works without a global install.
- `ESP32_WiFi_Connect` also needs ESPAsyncWebServer + AsyncTCP, but the Arduino index versions (ESPAsyncWebServer 3.1.0, AsyncTCP 1.1.4) fail on ESP32 core 3.x with `mbedtls_md5_*_ret` errors. Use the maintained ESP32Async forks instead: `arduino-cli lib uninstall ESPAsyncWebServer AsyncTCP ESPAsyncTCP`, then `git clone --depth 1 https://github.com/ESP32Async/AsyncTCP.git ~/Arduino/libraries/AsyncTCP` and `git clone --depth 1 https://github.com/ESP32Async/ESPAsyncWebServer.git ~/Arduino/libraries/ESPAsyncWebServer`. Verified with AsyncTCP 3.6.1 and ESPAsyncWebServer 3.13.0 in the default user libraries directory.
- Compile (verified, from the repo root): `arduino-cli compile --fqbn esp32:esp32:esp32 firmware/ComfortRoom` — generic ESP32 Dev Module; change the FQBN if the board changes.
- Upload: `arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 firmware/ComfortRoom`
- Serial Monitor: 115200 baud.
- The sketch path is `firmware/ComfortRoom/ComfortRoom.ino`; Arduino requires the folder name (`ComfortRoom`) to match the `.ino`, so do not flatten it to `firmware/ComfortRoom.ino`.

## Dashboard (`dashboard/`)

- SolidJS + Vite, source in `dashboard/src/` (`src/App.jsx` owns the store, MQTT lifecycle, and commands). Vite entry is `dashboard/index.html`; MQTT.js is bundled from npm, not a CDN.
- Toolchain comes from `dashboard/flake.nix` / `dashboard/flake.lock`: `nix develop ./dashboard` provides Node 22 and npm. JavaScript dependencies are pinned by `dashboard/package.json` + `dashboard/package-lock.json`; `node_modules/` and `dist/` are generated and gitignored.
- Dev server (port 8080): `cd dashboard && npm install && npm run dev`. Do not open `index.html` from `file://`; MQTT over WebSockets needs an HTTP origin.
- Production build: `cd dashboard && npm run build` (static site in `dashboard/dist/`). The dashboard flake also builds it: `nix build ./dashboard#dashboard` (or `cd dashboard && nix build`), then serve the resulting `result/` (e.g. `python3 -m http.server 8080 --directory result`).
- After changing JS dependencies, run `npm install` to update `dashboard/package-lock.json` and refresh `npmDepsHash` in `dashboard/flake.nix` with `nix run nixpkgs#prefetch-npm-deps -- dashboard/package-lock.json`.
- MQTT runs on the local `mosquitto.service`: firmware on `<host>:1883` (plain TCP), browsers on `ws://<page-host>:9001/mqtt` (WebSockets). The dashboard derives the host from the page origin; the firmware uses the LAN IP in `mqtt_server`. Both listeners must bind `0.0.0.0` and be allowed through the firewall; on this host they live in `/home/fr/nixos/configuration.nix` (`sudo nixos-rebuild switch --flake /etc/nixos#uzi`). The NixOS mosquitto module always loads an ACL plugin, and an empty ACL denies all traffic, so each listener also needs `acl = [ "topic readwrite #" ]`.
- `cd dashboard && npm run simulate` publishes synthetic retained telemetry (`NORMAL` → `HOT` → `ENERGY_WASTE`) to the local broker without an ESP32. Override with `MQTT_URL`, `DEVICE_ID`, `INTERVAL_MS`, `MOTION_HOLD_MS`; `Ctrl+C` publishes availability `offline`.
- `dashboard/src/constants.js` hardcodes the default device ID (`DEFAULT_DEVICE_ID`). Changing `DEVICE_ID` in the sketch requires changing that default too; per-browser broker/device overrides persist in `localStorage`.
- The dashboard owns all human-readable wording. Add status, error, and command mappings in `dashboard/src/constants.js` and use them in the components in the same release as the firmware change.

## Conventions

- Identifiers, serial log text, MQTT topics and payloads are English; keep new ones consistent. Payloads are **codes**, not prose (see the contract below).
- WiFi credentials are configured at runtime by `ESP32_WiFi_Connect`, never in the sketch: `wifiConnect.begin()` (setup) loads NVS credentials, or starts the `EasyESP-<chipid>` AP with a config page at `http://EasyESP-<chipid>.local/` or `http://192.168.4.1/`. The library always runs AP+STA, and saving credentials reboots the board. Do not add `ssid`/`password` constants or call `setDefaultWiFi()` with real credentials.
- `wifiConnect.keepAlive()` must run every `loop()` iteration — it drives the portal's DNS server. The library only retries the STA connection inside `begin()`, so `loop()` calls `WiFi.reconnect()` every 30 s when a configured station drops. MQTT connection attempts are separately rate-limited to every 3 s; do not restore the old blocking reconnect loop or the portal becomes unresponsive.
- LDR polarity is simulator/hardware-dependent: `lightLevel < lightThreshold` means bright (see `roomIsBright`).
- Internal state is typed (the `RoomStatus` enum, `bool` flags); only the MQTT wire format is text. `statusCode()` is the only mapping used on the air; `statusDescription()` is for the Serial Monitor and must never be published. Never send an enum value or a human-readable sentence over MQTT.
- Topics are compile-time concatenations via `TOPIC("...")`; avoid runtime `String` topic building.
- `client.setBufferSize(512)` is deliberate (prefixed topics plus acks exceed the 256-byte default); raise it if topics or payloads grow. Acks are queued (`ACK_QUEUE_SIZE` 4) and flushed from `loop()`, never published from inside the MQTT callback.

## MQTT contract (an external dashboard depends on these exact names)

Every topic is namespaced by device: `<id>/room/...`, where `<id>` is `DEVICE_ID` (`comfortroom-<uuid>`). The broker is public and shared, so the prefix is the only thing isolating this device. Use a distinct UUID per physical unit.

Publishes (all `retain = true` except `ack`):

- `<id>/room/temperature`, `<id>/room/humidity`, `<id>/room/light` — every `publishIntervalMs`
- `<id>/room/motion` — `ON` / `OFF` (binary, no `true`/`false`), from the `someonePresent` hold rather than raw PIR, so it lags the sensor
- `<id>/room/status` — `NORMAL` / `HOT` / `ENERGY_WASTE`, only when the value changes (reconnect and `publish ON` force a resend)
- `<id>/room/availability` — `online` on connect, `offline` as the MQTT last will (LWT)
- `<id>/room/ack` — command confirmations, not retained; the device never subscribes here

There is no `alert` topic: the alert text was a pure function of the status, so it was removed rather than duplicated. All user-visible wording (state labels, error messages, units) is owned by the dashboard.

Subscribes to `<id>/room/control/#`:

- `<id>/room/control/buzzer` — `ON` / `OFF` (persistent mute; only `ON` clears it)
- `<id>/room/control/silence` — `ON` / `OFF` (silences the current alarm; cleared on `NORMAL`)
- `<id>/room/control/temp_threshold` — float °C, `> 0` and `<= 100`
- `<id>/room/control/publish` — `ON` / `OFF` (also forces `status` resend on `ON`)
- `<id>/room/control/interval` — integer seconds, 1–3600
- `<id>/room/control/motion_hold` — integer seconds, 1–3600
- `<id>/room/control/light_threshold` — integer ADC, 0–4095

`ack` lives outside `control/#` on purpose: a subtopic of `control/#` would be delivered back to the device as a command. Ack payloads carry no prose:

- `<command>|OK` or `<command>|OK|<value>` — e.g. `buzzer|OK`, `temp_threshold|OK|32.0`, `interval|OK|5`
- `<command>|FAILED|<code>` — `E_TOKEN` (not `ON`/`OFF`), `E_NUMBER` (not numeric), `E_RANGE` (outside bounds), `E_UNKNOWN` (unknown subtopic), `E_TOPIC` (outside `control/#`)

The dashboard maps every code — status and error — to the wording it wants. Numeric payloads carry no unit; `°C` and friends are added by the dashboard. Adding a status or error code means adding it to the dashboard mapping in the same release.

Manual check: `mosquitto_sub -h 192.168.1.16 -v -t '<id>/room/#'` while publishing to a `control/...` topic with `mosquitto_pub`.

Do not rename topics or payload values without updating the dashboard.

## Control logic

Fusion sets `roomStatus` in priority order: `HOT` when temperature is above `tempThreshold` and raw PIR is `HIGH`; `ENERGY_WASTE` when bright with no motion for `noMotionHoldMs` (default 10 s); otherwise `NORMAL`. It is published as `NORMAL`, `HOT`, `ENERGY_WASTE`. Any `STATUS_NORMAL` iteration clears `buzzerSilenced` but keeps `buzzerMuted`, so the persistent mute survives alerts and only the dashboard can turn it off. The buzzer stays quiet while either flag is set.
