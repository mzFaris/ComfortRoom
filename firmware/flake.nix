{
  description = "ComfortRoom ESP32 firmware";

  inputs.nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.zst";

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = nixpkgs.legacyPackages.${system};
      });
    in
    {
      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          packages = [
            pkgs.arduino-cli
            pkgs.mosquitto
          ];
          shellHook = ''
            echo "ComfortRoom firmware: arduino-cli compile --fqbn esp32:esp32:esp32 ."
          '';
        };
      });
    };
}
