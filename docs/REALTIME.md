# Realtime milestone verification

Verified 2026-09-30 on the existing Arch user PipeWire 1.6.8 / WirePlumber session. No GUI/plugin work was undertaken.

## Actual chain

Selected physical source `alsa_input.pci-0000_00_1f.3.analog-stereo`, description **Built-in Audio Analog Stereo**, node 88, output ports 97/98 (FL/FR). Stable node name is persisted in the XDG config directory; numeric IDs below describe only this session.

The final normal daemon exported `skyapo.virtual_mic`, description **SkyAPO Virtual Mic**, node 174, media class Audio/Source. Capture input ports were 114/43. `pw-dump` observed active links 45/34 from physical node 88 to SkyAPO 174, and active links 134/154 from SkyAPO 174 to recording client node 124. The parallel raw-reference recording client was node 116. `wpctl status` listed the virtual source at unity volume.

Format: native planar float32 DSP ports, 48000 Hz, stereo FL/FR. Recording clients negotiated interleaved float32 48000 Hz stereo via normal PipeWire conversion. Observed quantum: 1024 frames (21.333 ms of graph time, **not** an end-to-end latency measurement). One actual upstream Preamp filter, config `examples/preamp.txt`, -6 dB. No additional latency estimate is claimed.

Two independent native recording streams explicitly target physical and virtual sources with fallback disabled. Each collected 192000 frames. Final normal and instrumented recordings both produced correlation 1, RMS amplitude ratio **0.501187**, gain **-6 dB**. Alignment lag was 0 frames in the final normal recording and 1024 frames in the instrumented recording. Lag is measured between these recording streams, not a calibrated hardware latency. WAVs are under ignored `build/*-raw.wav` / `build/*-processed.wav` and contain microphone audio.

An earlier implementation was visible but failed WirePlumber format negotiation; a client could fall back to the physical source, producing ratio 1. This was not accepted as success. The source now advertises Format/EnumFormat and acknowledges PortConfig transitions, and the test refuses fallback. Real consumption and numerical DSP gain were verified afterward.

The debounced inotify config watcher was subsequently tested live. The daemon loaded `tests/data/reload_minus6.txt`, then a replacement with `Preamp: -3 dB`; status reset per-chain meters and reported ratio 0.707946. An independent client then captured 192000 frames from the virtual source: correlation 1 and ratio 0.707946. Replacing the config with `UnsupportedDirective: true` produced a source/line reload error; status stayed streaming and an independent recording still measured ratio 0.707946. Callback allocation/deallocation counts remained zero. This verifies valid reload, bad reload rollback and real consumption of the retained graph.

After adding Channel/Copy routing, the normal PipeWire regression was repeated on the same physical device, and repeated with the ASan/UBSan daemon and recorder using `ASAN_OPTIONS=detect_leaks=0`. Both recordings independently consumed 192000 virtual-source frames with correlation 1 and RMS ratio 0.501187 (−6 dB). Runtime status reported zero audited callback allocations/deallocations and zero overruns. The sanitizer run shut down without memory-access diagnostics; disabling leak checking is needed only for the independently reproduced PipeWire context leak described below.

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
build/skyapo-realtime-probe alsa_input.pci-0000_00_1f.3.analog-stereo build/final-proof [-6]
build/skyapo-realtime-probe alsa_input.pci-0000_00_1f.3.analog-stereo build/reload-proof -3
```

Both builds succeeded; both CTest suites passed **3/3** (core, realtime allocation safety, offline WAV). After adding Channel/Copy, the latest normal CTest was 1.25 s and ASan/UBSan CTest 3.48 s. Core tests include actual upstream Preamp/BiQuad/IIR/Delay/Channel/Copy, nested Include, errors and varying blocks. Unsupported commands remain errors, not silently skipped.

Realtime allocation tests first verify allocator hooks detect deliberate allocations, then process 1000 variable-size blocks each in mono and stereo, plus 1000 blocks through Channel/Copy/Preamp/BiQuad/Delay routing: zero callback allocations/deallocations. Live status likewise reported **0 allocations / 0 deallocations**, and zero processing overruns in sampled normal/instrumented runs. Counters cover executable C++ and wrapped C allocation paths (including linked EAPO code), **not shared-library C allocator internals**. Buffers/pointer arrays/DSP states are initialized outside the callback. Process timing uses lock-free atomics and monotonic timestamps; logging/status serialization runs in the main loop.

Singleton startup was tested: a second daemon exits with an explicit lock error. Destroying only the SkyAPO node with `pw-cli destroy 170` caused reconnect after two seconds and recreated source node 174 with physical links. SIGINT teardown removed the source/socket; subsequent status explicitly reported `Daemon: not reachable`. The physical device/default source was not removed or rerouted. Full PipeWire-server restart and physical device unplug were not exercised.

### Sanitizer caveat

Unit/offline/allocation CTest ran with leak checking enabled and passed. Real PipeWire daemon/probe teardown triggers LeakSanitizer reports in installed module/context initialization. The standalone `skyapo-pipewire-lifetime` diagnostic, containing only PipeWire init/context/connect/disconnect/destroy/deinit and no SkyAPO/EAPO code, reproduces **3677 bytes / 34 allocations**. Daemon teardown reported 7354 bytes / 68 allocations (two contexts); the two-stream probe reported 3709 bytes / 34 allocations. This is an observed dependency-lifetime issue, not a clean realtime LeakSanitizer pass.

The realtime daemon, live graph reload, and recording test were subsequently run with `ASAN_OPTIONS=detect_leaks=0`; address/undefined behavior instrumentation remained enabled, the reloaded recording passed at -3 dB, and shutdown exited 0 without ASan/UBSan memory-access diagnostics. No blanket suppression was added to project builds or CTest. Investigate the installed PipeWire modules' leaks separately.

### Current realtime revalidation

On 2026-09-30 the selected source was `alsa_input.pci-0000_00_1f.3.analog-stereo`, runtime node 88 (Built-in Audio Analog Stereo). PipeWire negotiated F32 planar DSP, 48 kHz, stereo FL/FR, quantum 1024. `SkyAPO Virtual Mic` appeared as `Audio/Source`, node 174. `skyapo-realtime-probe` captured 192000 frames from both the physical device and virtual source: lag 1024 frames, correlation 1, and measured amplitude ratio 0.501187 for the `Preamp: -6 dB` example. The daemon reported 0 callback allocations, 0 callback deallocations, and 0 overruns. The same two-client capture succeeded with the ASan/UBSan daemon (`detect_leaks=0` for the known external PipeWire context leak).

Fresh revalidation after the Qt/device-selector and config-parser changes used a foreground daemon with `tests/data/reload_minus6.txt` (the user config was not changed). `wpctl status` showed physical node 88 and `SkyAPO Virtual Mic` node 174 simultaneously; `pw-dump` showed active physical-to-SkyAPO links 114 and 45 (FL/FR). Both no-fallback native recorder streams negotiated interleaved F32, 48000 Hz, stereo and captured 192000 frames. Measured correlation was 1, alignment lag −1024 frames, and RMS ratio 0.501187 (−6 dB). During the run, `skyapo status` reported quantum 1024, one active Preamp filter, 2/2 capture links, zero overruns, and zero audited callback allocations/deallocations. The daemon stopped cleanly and removed its virtual source; generated microphone WAVs were deleted after measurement. The lag is recorder alignment, not a calibrated latency value.

The same independent two-client hardware test was run with the ASan/UBSan daemon and probe using `ASAN_OPTIONS=detect_leaks=0` (LeakSanitizer only). Both streams captured 192000 frames at F32/48 kHz/stereo; correlation 1, lag 1024 frames, and RMS ratio 0.501187 (−6 dB). `pw-dump` observed active physical-to-virtual links 170 and 114; status reported 2/2 links, zero overruns, and zero audited callback allocations/deallocations. AddressSanitizer and UBSan remained enabled and emitted no memory-access/undefined-behavior diagnostics; the daemon removed the source on clean stop. Leak detection is disabled only for this real PipeWire process because the external PipeWire context/module leak is independently reproducible; normal sanitizer CTest retains leak checking.

The actual upstream Convolution and GraphicEQ implementations now execute in offline core tests. A separate allocation audit executes 1000 fixed-size upstream Convolution blocks with zero audited callback allocations/deallocations. Convolution is fixed-block: Engine rejects variable callback blocks; the daemon delays graph construction until negotiated quantum and handles rate/quantum changes on the control loop. The current physical-device recording exercise used Preamp, not a convolution IR. Exact PipeWire hardware tests of convolution at alternate rates/quantums remain outstanding.

### Graph-rate negotiation verification

The virtual source advertises planar float32 at 44.1, 48 and 96 kHz (with the selected source's channel positions), instead of pinning `SPA_PARAM_Format` to 48 kHz. On the same physical stereo source, PipeWire was temporarily configured to each rate, and the independent two-stream probe recorded both raw and processed sources at the selected rate with no fallback. Results with `Preamp: -6 dB`:

| Graph rate | Quantum | Frames per client | Correlation | RMS ratio | Callback alloc/free | Overruns |
|---|---:|---:|---:|---:|---:|---:|
| 44.1 kHz | 512 | 176400 | 1.0 | 0.501187 (−6 dB) | 0 / 0 | 0 |
| 48 kHz | 1024 | 192000 | 1.0 | 0.501187 (−6 dB) | 0 / 0 | 0 |
| 96 kHz | 2048 | 384000 | 1.0 | 0.501187 (−6 dB) | 0 / 0 | 0 |

After testing, PipeWire settings were restored to `clock.allowed-rates=[48000]` and `clock.force-rate=0`; status returned to 48 kHz / quantum 1024. These are observed graph quantum values, not end-to-end latency measurements. This verifies **stereo** at the three rates; a physical mono source has not been available for equivalent PipeWire recording.

### LV2 plugin in live graph

The test LV2 bundle was selected via `LV2_PATH` and loaded by `skyapod` using `tests/data/lv2_realtime.txt` (`Preamp: -6 dB` followed by the test plugin's default 0.5 gain). At 48 kHz stereo, a separate raw and virtual-source recording captured 192000 frames each, correlation 1 and measured ratio **0.250594** (−12.0206 dB, matching the combined DSP gain). Status showed two active filters, 0 callback allocations/deallocations, 0 overruns; the daemon was stopped and removed its virtual node afterward. This exercises one deliberately simple LV2 audio/control-port plugin, not a third-party plugin's safety or latency.

## Implementation file changes in this milestone

- `CMakeLists.txt`, `.gitignore`, `.gitmodules`, `cmake/PortableEapo.cmake`: pinned upstream gitlink, clean build-time portability adaptation, native runtime/audit/probe targets.
- `src/core/Engine.h`, `src/core/Engine.cpp`: explicit external channel names; relative/nested Include expansion; ordered upstream Channel/Copy channel routing with preallocated buses; transactional config construction; unchanged actual upstream DSP math.
- `src/platform/linux/ChannelHelper.cpp`: Linux channel-name implementation backing the upstream ChannelFilter/CopyFilter API.
- `src/platform/linux/ConvolutionFilterLinux.cpp`, `src/platform/linux/compat.cpp`, `src/platform/linux/stdafx.h`: Linux libsndfile IR loader and narrow upstream compatibility declarations.
- `src/pipewire/DeviceManager.h`, `src/pipewire/DeviceManager.cpp`: physical source/port registry enumeration.
- `src/pipewire/Runtime.h`, `src/pipewire/Runtime.cpp`: native capture links, source DSP ports, negotiation, buffers, rate rebuild, recovery, inotify hot reload, safe graph lifetime swap, telemetry and status socket.
- `src/platform/Settings.h`, `src/platform/PlatformChannels.h`, `src/platform/RealtimeAudit.h`, `src/platform/RealtimeAudit.cpp`: XDG selection/config/IPC/singleton, SPA-to-EAPO names, allocator audit.
- `src/daemon/main.cpp`, `src/cli/main.cpp`: real daemon lifecycle and device/status commands.
- `tests/realtime_probe.cpp`, `tests/realtime_safety_tests.cpp`, `tests/pipewire_lifetime.cpp`, `tests/data/reload_*.txt`: recording with selectable expected gain, realtime allocation, dependency lifetime and reload fixtures.
- `README.md`, `ROADMAP.md`, `CHANGELOG.md`, `docs/PORTING.md`, `docs/REALTIME.md`, `docs/CONFIG.md`, `docs/ARCHITECTURE.md`, `docs/BUILDING.md`, `docs/PLUGINS.md`, `docs/UI.md`, `docs/TROUBLESHOOTING.md`: staged product plan and current usage/maintenance/compatibility notes.
- `upstream/equalizerapo/filters/BiQuad.h`: previous local modification removed; upstream checkout now clean. CMake also compiles the actual upstream ChannelFilter and CopyFilter implementations.

The initial implementation was committed as `a7b187f`; the config/reload milestone is a subsequent commit. The upstream Git directory is absorbed into `.git/modules/upstream/equalizerapo`; only a mode-160000 gitlink is tracked. See `GIT-STATUS.md` for current root status.

## Remaining limitations / next task

The end-to-end hardware chain and transactional hot reload are proven, with no missing audio environment blocker. Channel and Copy pass offline expected-sample and callback-allocation tests, but their output-channel expansion is rejected because the PipeWire virtual node has a fixed layout. Full upstream FilterEngine/FilterConfiguration integration and complete parser compatibility remain unimplemented. Convolution hardware/rate variation, other device layouts, and PipeWire server restart need testing. Source gain control is fixed unity. No plugin/GUI work was added. Highest-value next step: test fixed-block convolution through PipeWire, then expand integration to more device layouts and formats.
