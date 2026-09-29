# Building

Requirements: CMake 3.20+, C++17 compiler, pkg-config, libsndfile and PipeWire development files. The daemon currently requires `libpipewire-0.3`; a PipeWire user session is needed for device/runtime operations. FFTW3f is optional at configure time and enables upstream GraphicEQ/Convolution. Arch example: `sudo pacman -S cmake gcc pkgconf libsndfile pipewire wireplumber fftw`.

```sh
git clone --recurse-submodules <SkyAPO-repository>
cd LinuxAPO
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Sanitizer build: `cmake -S . -B build-asan -DSKYAPO_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`, followed by build and CTest. A known PipeWire module/context teardown leak is reproduced by the standalone minimal diagnostic and documented in `REALTIME.md`; ordinary tests pass with leak checking enabled. CMake installs the executables, user service, documentation, and upstream license. The realtime recording probe is manual because CI machines do not have to expose audio hardware.
# Arch Linux packaging

The repository provides `packaging/PKGBUILD`, which builds from the local Git checkout and initializes the pinned official Equalizer APO submodule. From the repository root:

```sh
cd packaging
makepkg -si
skyapo device set <node-name>
systemctl --user enable --now skyapod
```

The installed user unit is `skyapod.service`; it starts after the user's PipeWire and WirePlumber units, restarts on failure, and runs without root. To remove the service, use `systemctl --user disable --now skyapod` before removing the package.
