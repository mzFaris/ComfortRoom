# ComfortRoom dashboard

SolidJS + Vite browser dashboard for the ComfortRoom ESP32. It subscribes to the
firmware's MQTT topics, shows the live room state, and publishes the control
commands documented in [AGENTS.md](../AGENTS.md).

MQTT.js is bundled from npm (no CDN), so the dashboard works offline once built
or while the dev server is running.

## Toolchain

Node 22 and npm come from this directory's Nix flake. From the repository root:

```bash
nix develop ./dashboard
```

Or from this directory:

```bash
nix develop
```

JavaScript dependencies are pinned in `package.json` and `package-lock.json`.
The firmware has its own separate flake at `firmware/flake.nix`.

## Run

```bash
cd dashboard
npm install
npm run dev
```

Open <http://localhost:8080>. The dev server must be reached over HTTP, not by
opening `index.html` from `file://`, because MQTT over WebSockets needs a normal
browser origin.

## Build

```bash
cd dashboard
npm run build        # static site in dist/
npm run preview      # serve dist/ on port 8080
```

Or build the static site through Nix (from the repository root):

```bash
nix build ./dashboard#dashboard
python3 -m http.server 8080 --directory result
```

Or from this directory:

```bash
nix build
```

`dist/` and `node_modules/` are generated and gitignored. After changing
JavaScript dependencies, update the lockfile with `npm install` and refresh the
flake hash:

```bash
nix run nixpkgs#prefetch-npm-deps -- dashboard/package-lock.json
```

Put the resulting hash in `dashboard/flake.nix` (`npmDepsHash`).

## Simulate the device

No ESP32 required. This publishes synthetic telemetry to the local broker in a
loop, cycling through `NORMAL`, `HOT`, and `ENERGY_WASTE`:

```bash
cd dashboard
npm run simulate
```

Optional overrides:

```bash
MQTT_URL=mqtt://127.0.0.1:1883 \
DEVICE_ID=comfortroom-8f2c41d6-9a3b-4e57-b0d2-6c5a7e19f408 \
INTERVAL_MS=3000 \
npm run simulate
```

`Ctrl+C` publishes `availability=offline` before exiting.

## Connect

The defaults assume a broker provided by the host (see
[AGENTS.md](../AGENTS.md)); nothing in this repo runs one:

| Setting | Default |
|---|---|
| Broker | `ws://<page-host>:9001/mqtt` |
| Device ID | the `DEVICE_ID` in `firmware/ComfortRoom/ComfortRoom.ino` |

The firmware uses plain MQTT on port 1883; browsers use MQTT over WebSockets on
port 9001, so the broker needs both listeners, for example:

```conf
listener 1883 0.0.0.0
allow_anonymous true

listener 9001 0.0.0.0
protocol websockets
allow_anonymous true
```

Configure those listeners wherever the broker runs; this repo does not manage
it. The NixOS `services.mosquitto` module additionally loads an ACL plugin, so
each listener needs `acl = [ "topic readwrite #" ]`; an empty ACL denies all
traffic.

The page connects automatically. Use **Settings** to change the broker or device
ID; both are remembered in `localStorage`.

## Monitoring

- Fused status: `NORMAL` → "Normal", `HOT` → "Room too hot",
  `ENERGY_WASTE` → "Energy waste". Unknown codes are shown verbatim so a newer
  firmware does not break the UI.
- Temperature, humidity and light ADC with a sparkline of the last 90 samples
  (session only, per metric).
- Motion (`ON`/`OFF` → Occupied/Empty), device availability (`online`/`offline`),
  and how long ago the last reading arrived. Readings are highlighted once they
  are older than expected (20 s with the default 3 s publish interval; the
  dashboard adapts to intervals it applies).
- Bright/dim is estimated from the light reading compared with the light
  threshold this browser last applied (the firmware treats readings below the
  threshold as bright).
- Activity log for status changes, availability, commands and acknowledgements.

## Controls

| UI | Topic | Payload |
|---|---|---|
| Silence | `control/silence` | `ON` |
| Unmute / Mute | `control/buzzer` | `ON` / `OFF` |
| Resume / Pause | `control/publish` | `ON` / `OFF` |
| Hot threshold | `control/temp_threshold` | float °C, `> 0` and `<= 100` |
| Publish interval | `control/interval` | integer seconds, 1–3600 |
| Motion hold | `control/motion_hold` | integer seconds, 1–3600 |
| Light threshold | `control/light_threshold` | integer ADC, 0–4095 |

Every command is confirmed on `room/ack`; the dashboard shows the result next to
the control, logs it, and warns after 6 s without an acknowledgement. Error codes
are mapped to plain text (`E_TOKEN`, `E_NUMBER`, `E_RANGE`, `E_UNKNOWN`,
`E_TOPIC`). Unknown codes stay visible instead of being hidden.

Things worth knowing:

- The firmware does not publish its current thresholds, so the fields start from
  the firmware defaults and reflect what this browser last applied. After a
  device reboot they are defaults again.
- `silence ON` stops the current alarm; the firmware clears the silence when the
  room returns to `NORMAL`, so the next alert sounds again.
- `buzzer OFF` is a persistent mute: it stays off across alerts until `buzzer ON`.
- `publish OFF` pauses sensor and status topics, but `ack` and `availability`
  keep flowing, so the dashboard can still tell that the device is alive.

## Caveats

- The broker is unauthenticated and exposed on ports 1883/9001.
  Anyone on the LAN who knows the device ID can read the room and send commands.
  Keep it on a trusted network and use a unique device UUID per unit.
- Retained values keep showing the last known readings while the device is
  offline; check the availability pill and the reading age.
- History is kept in memory only and disappears on reload.

## Source layout

| File | Purpose |
|---|---|
| `src/App.jsx` | Store, MQTT lifecycle, command handling, layout |
| `src/constants.js` | Status/error/command mappings and defaults |
| `src/components/` | Top bar, connection panel, hero, metrics, controls, activity |
| `src/styles.css` | Layout and theme |
| `vite.config.js` | Vite + Solid plugin, port 8080, relative asset base |
| `scripts/simulate.mjs` | Standalone MQTT telemetry simulator |
| `flake.nix` / `flake.lock` | Node toolchain and static-site build |
