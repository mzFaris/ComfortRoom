{
  inputs.nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.zst";

  outputs = { nixpkgs, ... }: {
    devShells = builtins.mapAttrs (system: pkgs: {
      default = pkgs.mkShell {

        packages = with pkgs; [
          arduino-cli
          bear
        ];
      };
    }) nixpkgs.legacyPackages;
  };
}
