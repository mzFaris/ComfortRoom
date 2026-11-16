# AGENTS.md

Single-file ESP32/Arduino sketch for a smart-room monitor (Wokwi simulation target). All code is in `ComfortRoom.ino`; there is no test suite, package manager, or CI.

## Build and upload

- Compile (verified): `arduino-cli compile --fqbn esp32:esp32:esp32 .` — generic ESP32 Dev Module; use a different FQBN if the target board changes.
- Prerequisites: `arduino-cli core install esp32:esp32` and `arduino-cli lib install "DHT sensor library" PubSubClient "Adafruit Unified Sensor"`.
- Upload (adjust port/board): `arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 .`
- `compile_commands.json` is gitignored and generated, not checked in: `bear -- arduino-cli compile --clean --fqbn esp32:esp32:esp32 .` (without `--clean`, the build cache makes Bear capture zero entries). Entries reference generated `.ino.cpp` files under `~/.cache/arduino/sketches/`.
- The sketch folder (`ComfortRoom`) must stay in sync with `ComfortRoom.ino`; renaming one requires renaming the other.

## Conventions

- Identifiers, comments, serial log text, MQTT topics and MQTT payloads are all English; keep new ones consistent. Payloads are **codes**, not prose (see the contract below).
- Simulation defaults: public broker `broker.hivemq.com:1883`. WiFi credentials are placeholders (`YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD`) — fill them locally, use `Wokwi-GUEST` with an empty password for the simulator, and never commit real credentials.
- LDR polarity is simulator/hardware-dependent: `lightLevel < lightThreshold` means bright (see the comment near `roomIsBright`).
- Internal state is typed (the `RoomStatus` enum, `bool` flags); only the MQTT wire format is text. `statusCode()` is the only mapping used on the air; `statusDescription()` is for the Serial Monitor and must never be published. Never send an enum value or a human-readable sentence over MQTT — user-visible text belongs to the dashboard.
- Topics are compile-time concatenations via `TOPIC("...")`; avoid runtime `String` topic building.

## MQTT contract (an external dashboard depends on these exact names)

Every topic is namespaced by device: `<id>/room/...`, where `<id>` is `DEVICE_ID` (`comfortroom-<uuid>`). The broker is public and shared, so the prefix is the only thing isolating this device. Use a distinct UUID per physical unit.

Publishes (all `retain = true` except `ack`):

- `<id>/room/temperature`, `<id>/room/humidity`, `<id>/room/light` — every `publishIntervalMs`
- `<id>/room/motion` — `ON` / `OFF` (binary, no `true`/`false`), derived from the same `lastMotionMs` hold as the fusion, so PIR drops are not reported instantly
- `<id>/room/status` — `NORMAL` / `HOT` / `ENERGY_WASTE`, only when the value changes
- `<id>/room/availability` — `online` on connect, `offline` as the MQTT last will (LWT)
- `<id>/room/ack` — command confirmations, not retained; the device never subscribes here

There is no `alert` topic: the alert text was a pure function of the status, so it was removed rather than duplicated. All user-visible wording (state labels, error messages, units) is owned by the dashboard.

Subscribes to `<id>/room/control/#`:

- `<id>/room/control/buzzer` — `ON` / `OFF`
- `<id>/room/control/temp_threshold` — float °C, `> 0` and `<= 100`
- `<id>/room/control/publish` — `ON` / `OFF` (also forces `status` resend on `ON`)
- `<id>/room/control/interval` — integer seconds, 1–3600
- `<id>/room/control/motion_hold` — integer seconds, 1–3600
- `<id>/room/control/light_threshold` — integer ADC, 0–4095

`ack` lives outside `control/#` on purpose: a subtopic of `control/#` would be delivered back to the device as a command. Ack payloads carry no prose:

- `<command>|OK` or `<command>|OK|<value>` — e.g. `buzzer|OK`, `temp_threshold|OK|32.0`, `interval|OK|5`
- `<command>|FAILED|<code>` — `E_TOKEN` (not `ON`/`OFF`), `E_NUMBER` (not numeric), `E_RANGE` (outside bounds), `E_UNKNOWN` (unknown subtopic), `E_TOPIC` (outside `control/#`)

The dashboard maps every code — status and error — to the wording it wants. Numeric payloads carry no unit; `°C` and friends are added by the dashboard. Adding a status or error code means adding it to the dashboard mapping in the same release.

Do not rename topics or payload values without updating the dashboard.

## Control logic

Fusion sets `roomStatus` to `STATUS_NORMAL`, `STATUS_HOT`, or `STATUS_ENERGY_WASTE`, published as `NORMAL`, `HOT`, `ENERGY_WASTE`. `STATUS_ENERGY_WASTE` means bright with no motion for `noMotionHoldMs` (10 s). Any `STATUS_NORMAL` iteration clears `buzzerMuted`.
