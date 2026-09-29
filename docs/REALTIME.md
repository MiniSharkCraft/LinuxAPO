# Realtime milestone verification

Verified 2026-09-30 on the existing Arch user PipeWire 1.6.8 / WirePlumber session. No GUI/plugin work was undertaken.

## Actual chain

Selected physical source `alsa_input.pci-0000_00_1f.3.analog-stereo`, description **Built-in Audio Analog Stereo**, node 88, output ports 97/98 (FL/FR). Stable node name is persisted in the XDG config directory; numeric IDs below describe only this session.

The final normal daemon exported `skyapo.virtual_mic`, description **SkyAPO Virtual Mic**, node 174, media class Audio/Source. Capture input ports were 114/43. `pw-dump` observed active links 45/34 from physical node 88 to SkyAPO 174, and active links 134/154 from SkyAPO 174 to recording client node 124. The parallel raw-reference recording client was node 116. `wpctl status` listed the virtual source at unity volume.

Format: native planar float32 DSP ports, 48000 Hz, stereo FL/FR. Recording clients negotiated interleaved float32 48000 Hz stereo via normal PipeWire conversion. Observed quantum: 1024 frames (21.333 ms of graph time, **not** an end-to-end latency measurement). One actual upstream Preamp filter, config `examples/preamp.txt`, -6 dB. No additional latency estimate is claimed.

Two independent native recording streams explicitly target physical and virtual sources with fallback disabled. Each collected 192000 frames. Final normal and instrumented recordings both produced correlation 1, RMS amplitude ratio **0.501187**, gain **-6 dB**. Alignment lag was 0 frames in the final normal recording and 1024 frames in the instrumented recording. Lag is measured between these recording streams, not a calibrated hardware latency. WAVs are under ignored `build/*-raw.wav` / `build/*-processed.wav` and contain microphone audio.

An earlier implementation was visible but failed WirePlumber format negotiation; a client could fall back to the physical source, producing ratio 1. This was not accepted as success. The source now advertises Format/EnumFormat and acknowledges PortConfig transitions, and the test refuses fallback. Real consumption and numerical DSP gain were verified afterward.

## Commands and results

```sh
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

cmake -S . -B build-asan -DSKYAPO_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure

build/skyapo device list
build/skyapo device set 88
build/skyapo device current
build/skyapod --config examples/preamp.txt
build/skyapo status
wpctl status
pw-dump
build/skyapo-realtime-probe alsa_input.pci-0000_00_1f.3.analog-stereo build/final-proof
```

Both builds succeeded; both CTest suites passed **3/3** (core, realtime allocation safety, offline WAV). Normal final CTest: 1.15 s; sanitizer CTest: 2.42 s. Core tests include actual upstream Preamp/BiQuad/IIR/Delay, errors and varying blocks. Unsupported commands remain errors, not silently skipped.

Realtime allocation tests first verify allocator hooks detect deliberate allocations, then process 1000 variable-size blocks each in mono and stereo, with Preamp/BiQuad/Delay/IIR: zero callback allocations/deallocations. Live status likewise reported **0 allocations / 0 deallocations**, and zero processing overruns in sampled normal/instrumented runs. Counters cover executable C++ and wrapped C allocation paths (including linked EAPO code), **not shared-library C allocator internals**. Buffers/pointer arrays/DSP states are initialized outside the callback. Process timing uses lock-free atomics and monotonic timestamps; logging/status serialization runs in the main loop.

Singleton startup was tested: a second daemon exits with an explicit lock error. Destroying only the SkyAPO node with `pw-cli destroy 170` caused reconnect after two seconds and recreated source node 174 with physical links. SIGINT teardown removed the source/socket; subsequent status explicitly reported `Daemon: not reachable`. The physical device/default source was not removed or rerouted. Full PipeWire-server restart and physical device unplug were not exercised.

### Sanitizer caveat

Unit/offline/allocation CTest ran with leak checking enabled and passed. Real PipeWire daemon/probe teardown triggers LeakSanitizer reports in installed module/context initialization. The standalone `skyapo-pipewire-lifetime` diagnostic, containing only PipeWire init/context/connect/disconnect/destroy/deinit and no SkyAPO/EAPO code, reproduces **3677 bytes / 34 allocations**. Daemon teardown reported 7354 bytes / 68 allocations (two contexts); the two-stream probe reported 3709 bytes / 34 allocations. This is an observed dependency-lifetime issue, not a clean realtime LeakSanitizer pass.

The realtime daemon and recording test were subsequently run with `ASAN_OPTIONS=detect_leaks=0`; address/undefined behavior instrumentation remained enabled, recording passed with the gain above, and shutdown exited 0 without ASan/UBSan memory-access diagnostics. No blanket suppression was added to project builds or CTest. Investigate the installed PipeWire modules' leaks separately.

## Implementation file changes in this milestone

- `CMakeLists.txt`, `.gitignore`, `.gitmodules`, `cmake/PortableEapo.cmake`: pinned upstream gitlink, clean build-time portability adaptation, native runtime/audit/probe targets.
- `src/core/Engine.h`, `src/core/Engine.cpp`: explicit external channel names; unchanged actual upstream DSP math.
- `src/pipewire/DeviceManager.h`, `src/pipewire/DeviceManager.cpp`: physical source/port registry enumeration.
- `src/pipewire/Runtime.h`, `src/pipewire/Runtime.cpp`: native capture links, source DSP ports, negotiation, buffers, rate rebuild, recovery, telemetry and status socket.
- `src/platform/Settings.h`, `src/platform/PlatformChannels.h`, `src/platform/RealtimeAudit.h`, `src/platform/RealtimeAudit.cpp`: XDG selection/config/IPC/singleton, SPA-to-EAPO names, allocator audit.
- `src/daemon/main.cpp`, `src/cli/main.cpp`: real daemon lifecycle and device/status commands.
- `tests/realtime_probe.cpp`, `tests/realtime_safety_tests.cpp`, `tests/pipewire_lifetime.cpp`: recording/gain, realtime allocation, dependency lifetime diagnostics.
- `README.md`, `docs/PORTING.md`, `docs/REALTIME.md`: usage, upstream maintenance and honest verification results.
- `upstream/equalizerapo/filters/BiQuad.h`: previous local modification removed; upstream checkout now clean.

Root Git was initialized during this milestone, so there is no pre-existing root commit against which to compute a historical diff. The upstream Git directory was absorbed into `.git/modules/upstream/equalizerapo`; only a mode-160000 gitlink is tracked. No commit was created. See `GIT-STATUS.md` for final root status/diff summary.

## Remaining limitations / next task

The end-to-end hardware milestone is proven, with no missing audio environment blocker. Full upstream FilterEngine/FilterConfiguration integration and hot config reload remain unimplemented, as do Channel/Copy/Include and advanced filters. Other hardware/layouts/rates and server restart need testing. Source gain control is fixed unity. No plugin/GUI work was added. Highest-value next step: transactional config hot reload retaining the last valid chain, with destruction deferred outside realtime processing.
