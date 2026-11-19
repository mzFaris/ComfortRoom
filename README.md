# ComfortRoom

ESP32 room-comfort monitor with a browser dashboard. The firmware reads
temperature/humidity (DHT11), motion (PIR) and light (LDR), fuses them into a
room status, and publishes it over MQTT. A SolidJS dashboard subscribes to the
same topics, charts the readings, and sends control commands back.

```
firmware/ComfortRoom/   ESP32 sketch (Arduino)
dashboard/              SolidJS + Vite dashboard
containers/             Podman images for the firmware toolchain, dashboard, broker
scripts/                Podman entry points for development and deployment
deploy/                 Quadlet units for a systemd user deployment
```

Everything runs through Podman. Only `podman` is required on the host; Node,
`arduino-cli` and Mosquitto live in images.

## Quick start

Firmware — the first run builds the toolchain image (ESP32 core + libraries), so
it takes a while and downloads about 2 GB; the resulting image is roughly 10 GB
because it carries the ESP32 toolchains for every target.

```bash
scripts/firmware.sh compile
scripts/firmware.sh upload     # /dev/ttyUSB0; override with TTY_DEV=...
scripts/firmware.sh monitor
```

Dashboard:

```bash
scripts/mqtt.sh up             # broker: 1883 (MQTT) + 9001 (WebSockets)
scripts/dashboard.sh dev       # http://localhost:8080
```

No ESP32 at hand? `scripts/dashboard.sh simulate` publishes synthetic telemetry
to the local broker.

Deployment — build the image and install the broker + dashboard as systemd user
services (Podman Quadlets):

```bash
scripts/deploy.sh install      # start now and on login
scripts/deploy.sh status
scripts/deploy.sh uninstall
```

## MQTT

Every topic is namespaced by device ID (`comfortroom-<uuid>`):

- publishes `<id>/room/{temperature,humidity,light,motion,status,availability,ack}`
- subscribes to `<id>/room/control/{buzzer,silence,publish,interval,motion_hold,temp_threshold,light_threshold}`

Payloads are machine-readable codes, not prose; the dashboard owns all
human-readable wording. The exact contract is in [`AGENTS.md`](AGENTS.md).

The local broker is `containers/mosquitto/mosquitto.conf`, run by
`scripts/mqtt.sh up`: plain MQTT on `1883` for the device and MQTT over
WebSockets on `9001` for browsers. Point `mqtt_server` in the sketch at the
machine running the broker and open both ports in its firewall. The example
broker is unauthenticated on a trusted LAN — use a unique device UUID per unit.

## More

- [`AGENTS.md`](AGENTS.md) — build/flash commands, MQTT contract, control logic
- [`dashboard/README.md`](dashboard/README.md) — dashboard setup, build, caveats
