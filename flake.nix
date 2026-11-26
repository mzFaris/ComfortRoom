{
  description = "ComfortRoom - ESP32 firmware build environment";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.zst";
  };

  outputs =
    {
      self,
      nixpkgs,
    }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        packages = [
          # arduino-cli from nixpkgs-unstable.
          pkgs.arduino-cli

          # The ESP32 platform (3.3.12) shells out to python3 for
          # gen_esp32part.py during the build.
          pkgs.python3
        ];

        shellHook = ''
          # Keep the core/libraries inside the repo: $HOME is not writable in
          # some environments and the ESP32 core is several GB.
          export ARDUINO_DIRECTORIES_DATA="$PWD/.arduino/data"
          export ARDUINO_DIRECTORIES_USER="$PWD/.arduino/user"
          export ARDUINO_DIRECTORIES_DOWNLOADS="$PWD/.arduino/staging"
          export ARDUINO_CONFIG_FILE="$PWD/.arduino/arduino-cli.yaml"
          mkdir -p "$ARDUINO_DIRECTORIES_DATA" "$ARDUINO_DIRECTORIES_USER"

          # First-run setup: board index, ESP32 core, and the libraries the
          # sketch needs. Each step is skipped when already present, so
          # entering the shell stays cheap after the first time.
          if [ ! -f "$ARDUINO_CONFIG_FILE" ]; then
            echo "arduino-cli: initialising config in $ARDUINO_CONFIG_FILE"
            arduino-cli config init >/dev/null
            arduino-cli config add board_manager.additional_urls \
              https://espressif.github.io/arduino-esp32/package_esp32_index.json >/dev/null
          fi

          if ! arduino-cli core list 2>/dev/null | grep -q '^esp32:esp32[[:space:]]\+3\.3\.12'; then
            echo "arduino-cli: installing esp32:esp32@3.3.12 (downloads ~1 GB, one-time)"
            arduino-cli core update-index
            arduino-cli core install esp32:esp32@3.3.12
          fi

          install_lib() {
            local name="$1" version="$2"
            if arduino-cli lib list 2>/dev/null \
              | awk -v name="$name" 'NR > 1 && index($0, name) == 1 { found = 1 } END { exit !found }'; then
              return 0
            fi
            echo "arduino-cli: installing $name@$version"
            arduino-cli lib install "$name@$version"
          }

          install_lib "DHT sensor library" "1.4.7"
          install_lib "ESP32_WiFi_Connect" "1.3.2"
          install_lib "PubSubClient" "2.8.0"
          install_lib "ESP Async WebServer" "3.12.1"
          install_lib "Async TCP" "3.6.1"
        '';
      };
    };
}
