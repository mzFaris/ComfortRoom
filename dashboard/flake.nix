{
  description = "ComfortRoom SolidJS dashboard";

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
          packages = [ pkgs.nodejs_22 ];
          shellHook = ''
            echo "ComfortRoom dashboard: npm install && npm run dev"
          '';
        };
      });

      packages = forAllSystems ({ pkgs, ... }:
        let
          dashboard = pkgs.buildNpmPackage {
            pname = "comfortroom-dashboard";
            version = "0.1.0";
            nodejs = pkgs.nodejs_22;
            src = pkgs.lib.cleanSourceWith {
              src = ./.;
              filter = path: type:
                let base = baseNameOf (toString path);
                in base != "node_modules" && base != "dist"
                  && base != "result" && base != "flake.nix"
                  && base != "flake.lock";
            };
            npmDepsHash = "sha256-PORqVNKB70QAx1zutz02GSmSGPjiwz+Pgkx+hJhpzBA=";
            installPhase = ''
              runHook preInstall
              mkdir -p $out
              cp -r dist/. $out/
              runHook postInstall
            '';
          };
        in
        {
          inherit dashboard;
          default = dashboard;
        });
    };
}
