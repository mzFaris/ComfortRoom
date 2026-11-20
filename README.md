# ComfortRoom

ESP32 room-comfort monitor with a browser dashboard. The firmware reads
temperature/humidity (DHT11), motion (PIR) and light (LDR), fuses them into a
room status, and publishes it over MQTT. A SolidJS dashboard subscribes to the
same topics, charts the readings, and sends control commands back.

```
firmware/ComfortRoom/   ESP32 sketch (Arduino)
dashboard/              SolidJS + Vite dashboard
```

## Quick start

Firmware (`arduino-cli` ships in the `firmware` flake, so no global install is
needed; see `AGENTS.md` for the one-time core/library setup):

```bash
nix develop ./firmware
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/ComfortRoom
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 firmware/ComfortRoom
```

Dashboard:

```bash
cd dashboard
npm install
npm run dev        # http://localhost:8080
```

No ESP32 at hand? `cd dashboard && npm run simulate` publishes synthetic
telemetry to a local broker.

## MQTT broker (host prerequisite)

Both the firmware and the dashboard need a broker on the host; nothing in this
repo provides one. Any broker that exposes plain MQTT on `1883` for the device
and MQTT over WebSockets on `9001` for browsers, both bound to `0.0.0.0`, will
do. Point `mqtt_server` in the sketch at that machine. The setup assumes an
unauthenticated broker on a trusted LAN (see the
[dashboard caveats](dashboard/README.md#caveats)) — use a unique device UUID per
unit.

### With Podman (example)

Mosquitto 2.x needs a config file to open the second (WebSocket) listener, and
the firmware publishes retained state, so give it a persistent data volume.
Create `~/.config/comfortroom/mosquitto.conf`:

```conf
listener 1883 0.0.0.0
allow_anonymous true

listener 9001 0.0.0.0
protocol websockets
allow_anonymous true

persistence true
persistence_location /mosquitto/data/
```

Then run the broker as a rootless container (`:Z` relabels the bind-mounted
config for SELinux):

```bash
podman run -d --name comfortroom-mqtt --restart=always \
  -p 1883:1883 -p 9001:9001 \
  -v ~/.config/comfortroom/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro,Z \
  -v comfortroom-mqtt-data:/mosquitto/data \
  docker.io/library/eclipse-mosquitto:2
```

Open both ports once (firewalld):

```bash
sudo firewall-cmd --permanent --add-port=1883/tcp --add-port=9001/tcp
sudo firewall-cmd --reload
```

The named volume keeps retained values across broker restarts; remove it with
`podman volume rm comfortroom-mqtt-data`.

### Autostart (optional)

`--restart=always` only restarts the container after a crash while it runs; a
rootless container does not come back by itself after a reboot. For that,
replace it with a user Quadlet. Stop the ad-hoc container first, since the unit
uses the same name and ports:

```bash
podman rm -f comfortroom-mqtt
```

Save `~/.config/containers/systemd/comfortroom-mqtt.container`:

```ini
[Unit]
Description=ComfortRoom MQTT broker

[Container]
Image=docker.io/library/eclipse-mosquitto:2
PublishPort=1883:1883
PublishPort=9001:9001
Volume=%h/.config/comfortroom/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro,Z
Volume=comfortroom-mqtt-data:/mosquitto/data

[Install]
WantedBy=default.target
```

Then enable it; lingering lets it start at boot without a login:

```bash
systemctl --user daemon-reload
systemctl --user enable --now comfortroom-mqtt.service
loginctl enable-linger "$USER"
```

### Check it

The firmware flake ships the Mosquitto clients:

```bash
nix develop ./firmware -c mosquitto_sub -h 127.0.0.1 -v -t '<id>/room/#'
```

Or publish synthetic telemetry from the dashboard with
`cd dashboard && npm run simulate`.

## MQTT

Every topic is namespaced by device ID (`comfortroom-<uuid>`):

- publishes `<id>/room/{temperature,humidity,light,motion,status,availability,ack}`
- subscribes to `<id>/room/control/{buzzer,silence,publish,interval,motion_hold,temp_threshold,light_threshold}`

Payloads are machine-readable codes, not prose; the dashboard owns all
human-readable wording. The exact contract is in [`AGENTS.md`](AGENTS.md).

## More

- [`AGENTS.md`](AGENTS.md) — build/flash commands, MQTT contract, control logic
- [`dashboard/README.md`](dashboard/README.md) — dashboard setup, build, caveats
