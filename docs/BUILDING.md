# Building

Requirements: CMake 3.20+, C++17 compiler, pkg-config, libsndfile and PipeWire development files. Initialize pinned source with `git submodule update --init --recursive`; the official CLAP API headers are a separate MIT-licensed submodule. The daemon currently requires `libpipewire-0.3`; a PipeWire user session is needed for device/runtime operations. FFTW3f is optional at configure time and enables upstream GraphicEQ/Convolution. Lilv is optional and enables LV2 hosting. Qt 6 Widgets is optional and enables `skyapo-ui` with selected upstream GUI components. Arch example: `sudo pacman -S cmake gcc pkgconf libsndfile pipewire wireplumber fftw lilv qt6-base`.

```sh
git clone --recurse-submodules <SkyAPO-repository>
cd LinuxAPO
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Launch the configuration editor with `build/skyapo-ui [config-file]`. CMake omits the target with a status message if Qt 6 Widgets is unavailable; use `-DSKYAPO_BUILD_UI=OFF` for a core-only build.

Sanitizer build: `cmake -S . -B build-asan -DSKYAPO_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`, followed by build and CTest. A known PipeWire module/context teardown leak is reproduced by the standalone minimal diagnostic. When Lilv is present, CTest uses the narrowly scoped `tests/lsan.supp` for Lilv 0.28.0's 24-byte plugin-class allocation; other leaks remain enabled/reported. Details are in `REALTIME.md`. CMake installs the executables, user service, documentation, and upstream license. The realtime recording probe is manual because CI machines do not have to expose audio hardware.
# Arch Linux packaging

The repository provides `packaging/PKGBUILD`, which builds from the local Git checkout and initializes the pinned official Equalizer APO submodule. From the repository root:

```sh
cd packaging
makepkg -si
skyapo device set <node-name>
systemctl --user enable --now skyapod
```

The installed user unit is `skyapod.service`; it starts after the user's PipeWire and WirePlumber units, restarts on failure, and runs without root. The PKGBUILD was built and tested locally with `makepkg --noconfirm --force`; it initializes the official pinned submodule and runs CTest during packaging. To remove the service, use `systemctl --user disable --now skyapod` before removing the package.
