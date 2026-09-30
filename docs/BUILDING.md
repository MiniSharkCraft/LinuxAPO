# Building

Requirements: CMake 3.20+, C++17 compiler, pkg-config, libsndfile and PipeWire development files. muParser is optional for builds but enables basic `If:` conditions; the Arch package depends on it. Disable condition support explicitly with `-DSKYAPO_ENABLE_MUPARSER=OFF`. Initialize pinned source with `git submodule update --init --recursive`; the official CLAP API headers are a separate MIT-licensed submodule. The daemon currently requires `libpipewire-0.3`; a PipeWire user session is needed for device/runtime operations. FFTW3f is optional at configure time and enables upstream GraphicEQ/Convolution. Lilv is optional and enables LV2 hosting. Qt 6 Widgets is optional and enables `skyapo-ui` with selected upstream GUI components. Arch example: `sudo pacman -S cmake gcc pkgconf libsndfile pipewire wireplumber muparser fftw lilv qt6-base`.

For deterministic end-to-end runtime tests, install the PipeWire daemon binary and use `-DSKYAPO_ENABLE_PIPEWIRE_E2E_TESTS=ON`. CTest starts isolated servers in temporary XDG runtime/config directories, processes deterministic mono and stereo sources with `Preamp: -6 dB`, and checks samples recorded by independent clients from `SkyAPO Virtual Mic`. The tests do not touch the desktop PipeWire server or require physical hardware. The option is off by default; ordinary builds retain unit/offline tests only.

```sh
git clone --recurse-submodules <SkyAPO-repository>
cd LinuxAPO
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Launch the configuration editor with `build/skyapo-ui [config-file]`. CMake omits the target with a status message if Qt 6 Widgets is unavailable; use `-DSKYAPO_BUILD_UI=OFF` for a core-only build.

Sanitizer build: `cmake -S . -B build-asan -DSKYAPO_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`, followed by build and CTest. A known PipeWire module/context teardown leak is reproduced by the standalone minimal diagnostic. When Lilv is present, CTest uses the narrowly scoped `tests/lsan.supp` for Lilv 0.28.0's 24-byte plugin-class allocation; other leaks remain enabled/reported. Details are in `docs/REALTIME.md` in the source checkout.

CMake installs the executables, user service, man pages, `README.md`, `ROADMAP.md`, `CHANGELOG.md`, `THIRD_PARTY_NOTICES.md`, and Equalizer APO GPL plus CLAP/VST3 license texts. The Arch package additionally copies those license texts to `/usr/share/licenses/skyapo/`. Other files under `docs/` (including `BUILDING.md`, `PORTING.md`, `REALTIME.md`, `CONFIG.md`, `ARCHITECTURE.md`, `PLUGINS.md`, `UI.md`, `MIGRATION.md`, and `TROUBLESHOOTING.md`) are source-checkout documentation and are not installed by current CMake rules. README references to those notes are plain paths rather than relative Markdown links so they do not become broken links in a staged/installed README. CTest verifies installed files in a staged installation. The realtime recording probe is manual because CI machines do not have to expose audio hardware.
# Arch Linux packaging

The repository provides `packaging/PKGBUILD`, which builds from the local Git checkout and initializes the pinned official Equalizer APO submodule. From the repository root:

```sh
cd packaging
makepkg -si
skyapo device set <node-name>
systemctl --user enable --now skyapod
```

The installed user unit is `skyapod.service`; it starts after the user's PipeWire and WirePlumber units, restarts on failure, and runs without root. When Qt is present, installation also provides AppStream metadata and the branded `skyapo` icon. No `.txt` MIME association is registered because EAPO configs use ordinary text extensions. The PKGBUILD initializes the official pinned submodule and runs CTest during packaging. To remove the service, use `systemctl --user disable --now skyapod` before removing the package.
