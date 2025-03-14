let
  pkgs = import <nixpkgs> { };
  kernel = pkgs.linuxPackages.kernel;
in pkgs.stdenv.mkDerivation {
  pname = "hid-elgato-streamdeckplus";
  version = "0.0.1";

  src = ./.;

  nativeBuildInputs = kernel.moduleBuildDependencies;
  hardeningDisable = [ "pic" "format" ];

  makeFlags = kernel.makeFlags ++ [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "INSTALL_MOD_PATH=$(out)"
  ];

  installTargets = [ "modules_install" ];
}
