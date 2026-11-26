# ComfortRoom

ESP32/Arduino firmware: DHT11 temperature/humidity, LDR, PIR, and buzzer,
reporting over MQTT.

## Prerequisites

- An ESP32 board, USB cable, and (usually) membership in the `dialout` group
  for `/dev/ttyUSB0` / `/dev/ttyACM0`.
- [Nix](https://nixos.org) with flakes enabled (`experimental-features =
  "nix-command flakes"`). The dev shell provides `arduino-cli` and `python3`
  from nixpkgs-unstable.
- The first `nix develop` installs the ESP32 core (`esp32:esp32@3.3.12`) and
  the required libraries. This downloads roughly 1 GB into `.arduino/`
  (gitignored), so it takes a while; later entries to the shell are instant.

<details>
<summary>Without Nix</summary>

Install `arduino-cli`, then:

```sh
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install "DHT sensor library@1.4.7" "ESP32_WiFi_Connect@1.3.2" \
  "PubSubClient@2.8.0" "ESP Async WebServer@3.12.1" "Async TCP@3.6.1"
```

</details>

## Compile

From the repository root (the directory name must match `ComfortRoom.ino`):

```sh
nix develop
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build .
```

The images are written to `build/` (e.g. `ComfortRoom.ino.bin`). Delete
`build/` for a clean rebuild.

## Upload

With the board connected:

```sh
nix develop
arduino-cli board list                 # find the port, e.g. /dev/ttyUSB0
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 --build-path build .
```

Replace `/dev/ttyUSB0` with the port reported by `arduino-cli board list`;
some boards show up as `/dev/ttyACM0`.
