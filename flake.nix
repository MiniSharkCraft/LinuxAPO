{
  description = "SkyAPO Linux PipeWire audio processor (CLI/daemon/renderer)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # These are the exact gitlink revisions recorded in this repository. Nix's
    # local flake source is not assumed to contain populated Git submodules.
    equalizerapo = {
      url = "git+https://git.code.sf.net/p/equalizerapo/code?rev=bbfcc3e5024cbb9d61ba75fc88d78605cc4c9687";
      flake = false;
    };
    clap = {
      url = "git+https://github.com/free-audio/clap.git?rev=a47f6badb49d948fd009998f28309cdab78979c9";
      flake = false;
    };
    vst3-base = {
      url = "git+https://github.com/steinbergmedia/vst3_base.git?rev=fcf9da0bd27a16f7f03773a3a39822f28f5c8477";
      flake = false;
    };
    vst3-pluginterfaces = {
      url = "git+https://github.com/steinbergmedia/vst3_pluginterfaces.git?rev=4f547e8e102b47de4a8b8aaf343c73b700786372";
      flake = false;
    };
    vst3-public-sdk = {
      url = "git+https://github.com/steinbergmedia/vst3_public_sdk.git?rev=586dc5e6c8012c3e4b01c79389375cbe96bdb1da";
      flake = false;
    };
    fst = {
      url = "git+https://git.iem.at/zmoelnig/FST.git?rev=647af068765b75867e3a28b4dd8991ab9ed47f7c";
      flake = false;
    };
    # Use the repository's known-good archive instead of SourceForge's
    # attachment endpoint, which can return a different payload.
    muparserx-archive = {
      url = "path:./packaging/muparserx_v3_0_1.zip";
      flake = false;
    };
  };

  outputs = inputs@{ self, nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      muparserx = inputs.muparserx-archive;

      mkSkyapo = { enableFstVst2 ? false }:
        pkgs.stdenv.mkDerivation {
          pname = if enableFstVst2 then "skyapo-fst-vst2" else "skyapo";
          version = "0.9.0";
          src = self;

          nativeBuildInputs = with pkgs; [ cmake pkg-config python3 ];
          buildInputs = with pkgs; [
            pipewire
            libsndfile
            fftwFloat
            lilv
            lv2
            muparser
          ];

          # Materialize submodules from their flake-locked inputs. This keeps
          # source acquisition deterministic whether `self` came from a local
          # path, a Git checkout without initialized submodules, or an archive.
          postPatch = ''
            install_upstream() {
              source="$1"
              destination="$2"
              rm -rf "$destination"
              mkdir -p "$destination"
              cp -a "$source"/. "$destination"/
            }
            install_upstream ${inputs.equalizerapo} upstream/equalizerapo
            install_upstream ${inputs.clap} upstream/clap
            install_upstream ${inputs.vst3-base} upstream/vst3/base
            install_upstream ${inputs.vst3-pluginterfaces} upstream/vst3/pluginterfaces
            install_upstream ${inputs.vst3-public-sdk} upstream/vst3/public.sdk
            ${pkgs.lib.optionalString enableFstVst2 ''
              install_upstream ${inputs.fst} upstream/fst
            ''}
          '';

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_INSTALL_BINDIR=bin"
            "-DCMAKE_INSTALL_LIBDIR=lib"
            "-DCMAKE_INSTALL_DATAROOTDIR=share"
            "-DCMAKE_INSTALL_DOCDIR=share/doc/SkyAPO"
            "-DCMAKE_INSTALL_MANDIR=share/man"
            "-DSKYAPO_BUILD_UI=OFF"
            "-DSKYAPO_ENABLE_FFTW3F=ON"
            "-DSKYAPO_ENABLE_MUPARSER=ON"
            "-DSKYAPO_ENABLE_MUPARSERX=ON"
            "-DSKYAPO_MUPARSERX_ARCHIVE=${muparserx}"
            "-DSKYAPO_ENABLE_PIPEWIRE_E2E_TESTS=OFF"
            "-DSKYAPO_ENABLE_FST_VST2_HOST=${if enableFstVst2 then "ON" else "OFF"}"
            "-DSKYAPO_BUILD_VST2_PROTOTYPE_TESTS=${if enableFstVst2 then "ON" else "OFF"}"
          ];

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ctest --output-on-failure
            runHook postCheck
          '';

          meta = with pkgs.lib; {
            description = "Equalizer APO DSP routed through PipeWire";
            license = licenses.gpl2Plus;
            platforms = [ system ];
            mainProgram = "skyapo";
          };
        };

      skyapo = mkSkyapo { };
      skyapoFstVst2 = mkSkyapo { enableFstVst2 = true; };
      packageSmoke = pkgs.runCommand "skyapo-package-smoke" { nativeBuildInputs = [ pkgs.coreutils ]; } ''
        ${skyapo}/bin/skyapo --version
        ${skyapo}/bin/skyapo --help >/dev/null
        test -x ${skyapo}/bin/skyapod
        set +e
        ${skyapo}/bin/skyapo-render >/dev/null 2>&1
        render_status=$?
        set -e
        test "$render_status" -eq 2
        mkdir -p "$out"
        touch "$out/verified"
      '';
    in {
      packages.${system} = {
        inherit skyapo;
        default = skyapo;
        skyapo-fst-vst2 = skyapoFstVst2;
      };

      checks.${system}.package-smoke = packageSmoke;

      apps.${system}.default = {
        type = "app";
        program = "${skyapo}/bin/skyapo";
        meta.description = "SkyAPO command-line client";
      };
    };
}
