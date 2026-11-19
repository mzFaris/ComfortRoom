# AGENTS.md

Two projects, each with its own container image: the ESP32 firmware in `firmware/ComfortRoom/` (single sketch) and a SolidJS + Vite dashboard in `dashboard/`. Toolchains, the broker and deployment all run through Podman; `podman` is the only host dependency (plus a USB serial port for flashing). No test suite, linter, or CI; a successful firmware compile and dashboard build are the only automated checks.

## Build, flash, verify

- Toolchain image: `containers/firmware/Containerfile` (Debian slim + `arduino-cli` + the sketch libraries, ~345 MB). The ESP32 core is **not** baked in: it lives in the `comfortroom-arduino-core` Podman volume, pinned by `CORE_VERSION` (default `4.0.0`) and installed by `scripts/firmware.sh` on first compile. `scripts/firmware.sh` builds `localhost/comfortroom-firmware:latest` on first use. No host `arduino-cli` install is needed. Without the sensor/MQTT libraries baked into the image the compile fails with `fatal error: DHT.h: No such file or directory`.
- `ESP32_WiFi_Connect` also needs ESPAsyncWebServer + AsyncTCP. The Arduino index versions (ESPAsyncWebServer 3.1.0, AsyncTCP 1.1.4) fail on ESP32 core 3.x with `mbedtls_md5_*_ret` errors, so the Containerfile uninstalls them and clones the maintained ESP32Async forks into the image instead (verified with AsyncTCP 3.6.1 / ESPAsyncWebServer 3.13.0 and core 4.0.0). Change the Containerfile to update them; do not hand-install libraries into the image.
- Compile (verified, from the repo root): `scripts/firmware.sh compile`. The first run downloads and installs the core (~1.5 GB download, ~7 GB in the `comfortroom-arduino-core` volume); later runs reuse it. The run mounts the repo plus the core and build-cache volumes — both volumes use `:z` because SELinux otherwise relabels them per container and blocks the next run. Generic ESP32 Dev Module; set `FQBN` to change the board.
- Upload: `scripts/firmware.sh upload` (board at `/dev/ttyUSB0`; override with `TTY_DEV=...`). The script passes the serial device into the container with `--device` and installs the core first if needed. If rootless Podman reports a permission error on the port, add `--group-add keep-groups` to the `run_tool` call in `scripts/firmware.sh`.
- Serial Monitor: `scripts/firmware.sh monitor` (115200 baud by default; override with `BAUD=...`).
- `scripts/firmware.sh shell` opens a shell inside the toolchain image. `scripts/firmware.sh core-status` lists the installed core; `core-reset` deletes the core volume so the next compile reinstalls the pin, and `CORE_VERSION=...` changes that pin. Compile artifacts are cached in the `comfortroom-firmware-cache` volume; remove it with `podman volume rm comfortroom-firmware-cache` if a build looks stale.
- The sketch path is `firmware/ComfortRoom/ComfortRoom.ino`; Arduino requires the folder name (`ComfortRoom`) to match the `.ino`, so do not flatten it to `firmware/ComfortRoom.ino`.

## Dashboard (`dashboard/`)

- SolidJS + Vite, source in `dashboard/src/` (`src/App.jsx` owns the store, MQTT lifecycle, and commands). Vite entry is `dashboard/index.html`; MQTT.js is bundled from npm, not a CDN.
- Toolchain: `containers/dashboard/Containerfile` builds the site with Node 22 (`node:22-alpine`) and serves it with `nginx:alpine` on port 8080. JavaScript dependencies are pinned by `dashboard/package.json` + `dashboard/package-lock.json`; `node_modules/` and `dist/` are generated and gitignored. Only Podman is required on the host.
- Dev server (port 8080): `scripts/dashboard.sh dev`. It runs in the Node container with `dashboard/` mounted, installs dependencies with `npm ci` when `node_modules/` is missing, and starts `vite --host 0.0.0.0`. Do not open `index.html` from `file://`; MQTT over WebSockets needs an HTTP origin.
- Production build: `scripts/dashboard.sh build` creates `localhost/comfortroom-dashboard:latest`; `scripts/dashboard.sh serve` runs it on port 8080. For an auto-starting deployment, `scripts/deploy.sh install` builds the image and installs the Quadlet units from `deploy/` into the systemd user session (`scripts/deploy.sh uninstall|status|logs`).
- After changing JS dependencies, run `npm install` inside the container (`scripts/dashboard.sh shell`) to update `dashboard/package-lock.json`; the next image build (`npm ci`) picks it up.
- MQTT runs in the `comfortroom-mqtt` container (`scripts/mqtt.sh up`), image `eclipse-mosquitto:2`, config `containers/mosquitto/mosquitto.conf`: firmware on `<host>:1883` (plain TCP), browsers on `ws://<page-host>:9001/mqtt` (WebSockets). The dashboard derives the host from the page origin; the firmware uses the LAN IP in `mqtt_server`. Both listeners bind `0.0.0.0`, so open ports 1883 and 9001 on the host firewall. The broker is unauthenticated; keep it on a trusted LAN.
- `scripts/dashboard.sh simulate` publishes synthetic retained telemetry (`NORMAL` → `HOT` → `ENERGY_WASTE`) to the local broker without an ESP32. Override with `MQTT_URL`, `DEVICE_ID`, `INTERVAL_MS`, `MOTION_HOLD_MS`; `Ctrl+C` publishes availability `offline`.
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

Manual check (no host Mosquitto clients needed):

```bash
podman run --rm --network host docker.io/library/eclipse-mosquitto:2 \
  mosquitto_sub -h 127.0.0.1 -v -t '<id>/room/#'
podman run --rm --network host docker.io/library/eclipse-mosquitto:2 \
  mosquitto_pub -h 127.0.0.1 -t '<id>/room/control/publish' -m ON
```

Do not rename topics or payload values without updating the dashboard.

## Control logic

Fusion sets `roomStatus` in priority order: `HOT` when temperature is above `tempThreshold` and raw PIR is `HIGH`; `ENERGY_WASTE` when bright with no motion for `noMotionHoldMs` (default 10 s); otherwise `NORMAL`. It is published as `NORMAL`, `HOT`, `ENERGY_WASTE`. Any `STATUS_NORMAL` iteration clears `buzzerSilenced` but keeps `buzzerMuted`, so the persistent mute survives alerts and only the dashboard can turn it off. The buzzer stays quiet while either flag is set.
