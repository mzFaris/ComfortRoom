# ComfortRoom

ESP32 smart-room monitor. Reads temperature/humidity (DHT11), light level (LDR) and motion (PIR), fuses them into one room state, drives a buzzer + LED, and reports everything over MQTT. An external dashboard can change thresholds, pause telemetry, and mute the buzzer at runtime.

Single-file Arduino sketch — no build system, no tests, no dependencies beyond the ESP32 core and a couple of Arduino libraries.

## Features

- **Sensor fusion** into three states instead of four raw numbers: `NORMAL`, `HOT`, `ENERGY_WASTE`.
- **Retained publishes + last will**, so a dashboard opened later still gets current values and can tell when the device dies.
- **Device-namespaced topics** (`comfortroom-<uuid>/room/...`) — safe on a shared public broker.
- **Code payloads, no prose.** All user-visible wording lives in the dashboard, so labels and language can change without reflashing.
- **Validated commands.** Bad input is rejected with an error code instead of being silently coerced to zero.

## Hardware

| Pin | Component | Mode | Notes |
|---|---|---|---|
| D14 | PIR (HC-SR501) | `INPUT` | `HIGH` = motion |
| D27 | DHT11 | data | Digital, needs the DHT library |
| D34 | LDR module | `ADC1` | 0–4095; polarity varies, see [Notes](#notes) |
| D26 | Buzzer | `OUTPUT` | Active high |
| D4 | LED | `OUTPUT` | External indicator, active high |

## Quick start

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "DHT sensor library" PubSubClient "Adafruit Unified Sensor"

# 1. edit the WiFi placeholders in ComfortRoom.ino
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 .
```

WiFi credentials ship as placeholders (`YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD`). For the Wokwi simulator use `"Wokwi-GUEST"` with an empty password.

Set a unique `DEVICE_ID` per physical unit — it is the only thing keeping your topics separate from everyone else's on `broker.hivemq.com`.

## Room states

| State | Condition | LED | Buzzer |
|---|---|---|---|
| `HOT` | temp > `tempThreshold` **and** motion detected | on | on, unless muted |
| `ENERGY_WASTE` | bright **and** no motion for `noMotionHoldMs` | on | on, unless muted |
| `NORMAL` | anything else | off | off, and clears the mute flag |

Evaluated top to bottom, so a hot occupied room reads `HOT` and, if the room is bright, becomes `ENERGY_WASTE` once the motion hold expires. Any `NORMAL` iteration clears a dashboard mute.

Default parameters: `tempThreshold` 30.0 °C, `lightThreshold` 2000 (ADC), `noMotionHoldMs` 10 s, `publishIntervalMs` 3 s.

## MQTT contract

Every topic is prefixed with the device ID. In the examples below, `ID=comfortroom-8f2c41d6-9a3b-4e57-b0d2-6c5a7e19f408`.

### Published

| Topic | Payload | Retained | When |
|---|---|---|---|
| `$ID/room/temperature` | `32.5` (°C) | yes | every `publishIntervalMs` |
| `$ID/room/humidity` | `70.0` (%) | yes | every `publishIntervalMs` |
| `$ID/room/light` | `1850` (ADC) | yes | every `publishIntervalMs` |
| `$ID/room/motion` | `ON` / `OFF` | yes | every `publishIntervalMs` |
| `$ID/room/status` | `NORMAL` / `HOT` / `ENERGY_WASTE` | yes | on change |
| `$ID/room/availability` | `online` / `offline` | yes | connect / LWT |
| `$ID/room/ack` | see below | no | after a command |

Numbers carry no unit and no decimal comma; formatting is the dashboard's job. `motion` uses the same motion hold as the fusion, so `OFF` is not reported the instant the PIR drops.

### Commands

Publish to `$ID/room/control/<command>`:

| Command | Payload | Range |
|---|---|---|
| `buzzer` | `ON` / `OFF` | — |
| `temp_threshold` | float °C | `> 0` and `<= 100` |
| `publish` | `ON` / `OFF` | pauses telemetry, keeps the connection |
| `interval` | integer seconds | 1–3600 |
| `motion_hold` | integer seconds | 1–3600 |
| `light_threshold` | integer ADC | 0–4095 |

`publish OFF` pauses the data topics (sensors and `status`); `ack` and `availability` keep flowing so the dashboard can still see the device is alive and commands are accepted.

### Acknowledgements

Every command produces exactly one message on `$ID/room/ack`:

```
<command>|OK               buzzer|OK
<command>|OK|<value>       temp_threshold|OK|32.0
<command>|FAILED|<code>    interval|FAILED|E_RANGE
```

Error codes: `E_TOKEN` (not `ON`/`OFF`), `E_NUMBER` (not numeric), `E_RANGE` (outside the table above), `E_UNKNOWN` (unknown subtopic), `E_TOPIC` (outside `control/#`).

`ack` deliberately lives outside `control/#` — a subtopic there would be delivered back to the device as a command.

### Try it

```bash
ID=comfortroom-8f2c41d6-9a3b-4e57-b0d2-6c5a7e19f408

mosquitto_sub -h broker.hivemq.com -v -t "$ID/room/#"

mosquitto_pub -h broker.hivemq.com -t "$ID/room/control/temp_threshold" -m 32
mosquitto_pub -h broker.hivemq.com -t "$ID/room/control/buzzer" -m OFF
mosquitto_pub -h broker.hivemq.com -t "$ID/room/control/interval" -m 5
```

## Dashboard mapping

Payloads are codes, so the dashboard owns the text. A minimal mapping:

```js
const status = {
  NORMAL: "Normal",
  HOT: "Room too hot",
  ENERGY_WASTE: "Energy waste",
};
const motion = { ON: "Occupied", OFF: "Empty" };
const error = {
  E_TOKEN: "Only ON/OFF is accepted",
  E_NUMBER: "Value must be numeric",
  E_RANGE: "Value is out of the allowed range",
  E_UNKNOWN: "Unknown command",
  E_TOPIC: "Invalid command topic",
};

function renderAck(payload) {
  const [command, result, detail] = payload.split("|");
  return result === "OK"
    ? `${command}: applied${detail ? ` (${detail})` : ""}`
    : error[detail] ?? `Failed: ${detail}`;
}
```

Translate these strings to whatever the end user should read — that is the whole point of shipping codes. Adding a status or error code in the firmware means adding it here in the same release.

## Notes

- **LDR polarity** is hardware/simulator dependent: `lightLevel < lightThreshold` is treated as bright. If your slider reads high when bright, invert the comparison near `roomIsBright` in `loop()`.
- **Credentials** stay local. Do not commit a filled-in `ssid`/`password`.
- **One UUID per device.** Two units sharing `DEVICE_ID` will fight over the same topics.
- **No `alert` topic.** It was a pure function of `status`, so it was removed rather than duplicated.
- **Language:** identifiers, comments, serial logs, topics and payloads are all English.
- The sketch is written for the Wokwi simulator but runs unchanged on real ESP32 hardware once the pin map and WiFi credentials match.
- `AGENTS.md` documents the conventions and contracts an automated contributor must follow.

## Project layout

```
ComfortRoom/
├── ComfortRoom.ino   # the entire firmware
├── AGENTS.md         # conventions + MQTT contract
└── README.md
```
