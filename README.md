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

Firmware (needs `arduino-cli`; see `AGENTS.md` for the one-time library setup):

```bash
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

## MQTT

Every topic is namespaced by device ID (`comfortroom-<uuid>`):

- publishes `<id>/room/{temperature,humidity,light,motion,status,availability,ack}`
- subscribes to `<id>/room/control/{buzzer,silence,publish,interval,motion_hold,temp_threshold,light_threshold}`

Payloads are machine-readable codes, not prose; the dashboard owns all
human-readable wording. The exact contract is in [`AGENTS.md`](AGENTS.md).

Point `mqtt_server` in the sketch at the machine running your broker, and make
sure that broker exposes plain MQTT on `1883` for the device and MQTT over
WebSockets on `9001` for browsers. The example configuration uses an
unauthenticated broker on a trusted LAN — use a unique device UUID per unit.

## More

- [`AGENTS.md`](AGENTS.md) — build/flash commands, MQTT contract, control logic
- [`dashboard/README.md`](dashboard/README.md) — dashboard setup, build, caveats
