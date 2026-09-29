# Building

Requirements: CMake 3.20+, C++17 compiler, pkg-config, libsndfile and PipeWire development files. The daemon currently requires `libpipewire-0.3`; a PipeWire user session is needed for device/runtime operations. Arch example: `sudo pacman -S cmake gcc pkgconf libsndfile pipewire wireplumber`.

```sh
git clone --recurse-submodules <SkyAPO-repository>
cd LinuxAPO
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Sanitizer build: `cmake -S . -B build-asan -DSKYAPO_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`, followed by build and CTest. A known PipeWire module/context teardown leak is reproduced by the standalone minimal diagnostic and documented in `REALTIME.md`; ordinary tests pass with leak checking enabled. The project has no install package or systemd unit yet. The realtime recording probe is manual because CI machines do not have to expose audio hardware.
