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

### Virtual-source loss recovery

On 2026-09-30, a separate `skyapod` instance used an isolated `XDG_CONFIG_HOME` and the selected stable device name `alsa_input.pci-0000_00_1f.3.analog-stereo` (runtime physical node 88). At F32 planar, 48 kHz, stereo FL/FR, quantum 1024, `wpctl status` showed `SkyAPO Virtual Mic` node 174 and the daemon reported 2/2 capture links. Destroying only node 174 with `pw-cli destroy 174` caused the daemon to report `virtual source disconnected`, exit that runtime instance and retry; within about three seconds node 116 appeared with the daemon back in `streaming`, 2/2 links and zero overruns/host callback allocations/deallocations. An independent `skyapo-realtime-probe` then recorded 192000 frames from both the physical source and recreated virtual source, measured correlation 1, RMS ratio 0.501187 (−6 dB), and alignment lag −1024 frames. The daemon was stopped cleanly and the generated microphone WAV files were removed. This proves virtual-node-loss recovery, not a PipeWire server restart or physical-device hotplug test.

### PipeWire server restart recovery

On 2026-09-30, an isolated PipeWire 1.6.9 server used the non-installed `skyapo-pipewire-test-source` fixture, which publishes a deterministic 440 Hz stereo source under the stable name `skyapo.test.input`. SkyAPO selected that name and processed it at F32 planar, 48 kHz, stereo FL/FR, quantum 1024 with `Preamp: -6 dB`; before restart status measured RMS ratio 0.501187, with 2/2 links, zero overruns and zero audited callback allocations/deallocations. Stopping only this private PipeWire server produced `connection error`; `skyapod` removed the dead runtime and retried while the test source exited. After restarting the private server and test source with the same node name, SkyAPO rediscovered runtime node 33, recreated `SkyAPO Virtual Mic` as node 37, restored both FL/FR links, and returned to `streaming`. Status again measured RMS ratio 0.501187 and zero overruns/host callback allocations/deallocations. `pw-link -l` confirmed both active input links. The independent consumer verification after restart is recorded below. Physical desktop audio/server state was not changed.

The fixtures can be built with `cmake --build build --target skyapo-pipewire-test-source skyapo-pipewire-test-consumer`; they are test-only and are not installed in the product package. Run the source with no arguments for stereo or `skyapo-pipewire-test-source --mono` for its stable mono test source. Run the consumer with no arguments for stereo or `skyapo-pipewire-test-consumer --mono` for a mono virtual mic.

### Mono layout and selected-source loss

The same fixture's `--mono` mode publishes one `MONO` channel under the stable name `skyapo.test.mono`. On a separate private PipeWire server, `skyapo device list` reported one channel; after `skyapo device set skyapo.test.mono`, `skyapod` exposed a mono `SkyAPO Virtual Mic` and linked `capture_MONO` to `input_MONO`. At 48 kHz / quantum 1024, status measured RMS ratio 0.501187 for `Preamp: -6 dB`, with 1/1 links, zero overruns and zero audited callback allocations/deallocations. Stopping the fixture removed the selected source; the daemon reported `selected capture device disappeared` and retried. Restarting the fixture with the same stable node name led it to rediscover the source, rebuild the mono virtual node and restore the single link; status again measured 0.501187. An independent native PipeWire consumer captured 143360 mono frames from the virtual mic and measured RMS ratio 0.999974 against the expected −6 dB output. After a private PipeWire server restart it captured a further 144384 frames and measured 0.999999. This validates mono processing and synthetic selected-source disappearance/reappearance, not physical USB/ALSA unplugging.

### PipeWire consumer churn

The non-installed `skyapo-pipewire-test-consumer` client explicitly links its input ports to the virtual source and accumulates samples in its PipeWire process callback; it does not need WirePlumber to create the test links. In stereo it completed two independent 144384-frame captures across separate client lifetimes, measuring expected-output RMS ratios 0.999993 and 1.000003 (expected absolute RMS 0.03543929 for the deterministic 0.1-amplitude sine after −6 dB). After the consumer exited, the daemon remained `streaming` with 2/2 capture links, zero overruns and zero audited callback allocations/deallocations. Mono captures are recorded above. This verifies direct native PipeWire client consumption and attach/detach/reattach, not automatic desktop policy selection.

### Live sample-rate and quantum renegotiation

With the same isolated PipeWire 1.6.9 server, deterministic stereo source, native consumer and `Preamp: -6 dB`, SkyAPO started at 48 kHz / quantum 1024. While `skyapod` remained running, setting the private server's `clock.force-quantum` to 512 caused Engine reinitialization at 48 kHz / 512; the consumer captured 143872 frames and measured RMS ratio 0.999980 against the expected processed output. Setting `clock.force-rate` to 44100 kept quantum 512; the consumer captured 132096 frames and measured 0.999987. Setting rate 96000 and quantum 2048 caused another rebuild; the consumer captured 286720 frames and measured 0.999970. At each stage daemon status remained `streaming`, reported the negotiated rate/quantum, 2/2 input links, zero overruns and zero audited callback allocations/deallocations; upstream DSP amplitude telemetry remained 0.501187 relative to the input. The private server was stopped afterward, so no desktop PipeWire settings needed restoring. These are live renegotiation tests with Preamp, not a claim that fixed-block Convolution was exercised through a quantum change.

### Explicit daemon process restart

On 2026-09-30, `skyapo start` and `skyapo restart` were exercised against the live desktop PipeWire server with isolated XDG configuration; separate PIDs `1726284` and `1726296` both returned to streaming with 2/2 links. The stronger acceptance used an isolated PipeWire 1.6.9 graph and deterministic `skyapo.test.input` source (node 33), with `Preamp: -6 dB`. Before restart, daemon PID `1771944` exposed `SkyAPO Virtual Mic` node 37 at F32 planar, 48 kHz, stereo FL/FR, quantum 1024; status measured amplitude ratio 0.501187, zero overruns and zero audited callback allocations/deallocations. `skyapo restart` created PID `1772247`, restored the same virtual source/format/links/filter and 0.501187 ratio. The independent native consumer then exited 0 after capturing 144384 stereo frames: RMS 0.03543952 versus expected 0.03543929, ratio 1.000006. Status remained streaming with 2/2 links and zero overruns/allocations after the consumer detached; `skyapo stop` and private server shutdown were clean. A separate physical-mic probe during changing ambient input produced correlation 0.199082 and was discarded; it is not used as gain evidence. Sustained-load overrun testing remains open.

The actual upstream Convolution and GraphicEQ implementations now execute in offline core tests. A separate allocation audit executes 1000 fixed-size upstream Convolution blocks with zero audited callback allocations/deallocations. Convolution is fixed-block: Engine rejects variable callback blocks; the daemon delays graph construction until negotiated quantum and handles rate/quantum changes on the control loop. The current physical-device recording exercise used Preamp, not a convolution IR. Exact PipeWire hardware tests of convolution at alternate rates/quantums remain outstanding.

### CLAP host in live graph

On 2026-09-30, `skyapod` loaded `tests/data/clap_realtime.txt` with `CLAP_PATH=$PWD/build/test-plugins/clap`: upstream `Preamp: -6 dB` followed by the native CLAP test plugin (`org.skyapo.test.gain`, stereo half-gain). The physical capture was `alsa_input.pci-0000_00_1f.3.analog-stereo` node 88; SkyAPO exposed `skyapo.virtual_mic` as **SkyAPO Virtual Mic** (runtime node 68 in the final run; node IDs are ephemeral). PipeWire negotiated planar float32, 48 kHz, stereo FL/FR and quantum 1024. `wpctl status` showed the source and `pw-dump` showed its node/ports. An independent two-stream native client captured 192000 frames from physical and virtual sources (no fallback): correlation 1, alignment −1024 frames, RMS ratio **0.250594**, gain **−12.0206 dB**, matching the combined −6 dB preamp and 0.5 CLAP plugin. Runtime status showed two filters, 2/2 capture links, zero overruns and **0 callback allocations / 0 callback deallocations** (same executable/wrapped-allocation scope as above). This is an end-to-end CLAP ABI/graph proof using SkyAPO's deterministic fixture, not third-party CLAP compatibility or plugin-isolation proof. The daemon was stopped cleanly after capture and the four generated microphone WAVs were removed.

After adding the CLAP parameter event path, the same live chain was rerun from `tests/data/clap_realtime_override.txt` with parameter ID `7=0.25`. The final independent two-stream run captured 192000 frames, correlation 1, alignment −1024 frames, measured RMS ratio **0.125297** and gain **−18.0412 dB** (−6 dB preamp plus plugin gain 0.25); runtime status showed zero callback allocations/deallocations and zero overruns. A preceding attempt had unstable low correlation, so it was discarded and repeated; only the repeated correlation-1 recording is counted as verification. The daemon was stopped and the temporary microphone WAVs were removed.

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

The physical-to-virtual hardware chain, transactional reload, LV2 fixture graph and CLAP fixture graph are proven with no missing audio-environment blocker. Channel and Copy pass offline expected-sample and callback-allocation tests, but output-channel expansion is rejected because the PipeWire virtual node has a fixed layout. Full upstream FilterEngine/config-parser compatibility remains incomplete. Convolution hardware/rate variation, other device layouts and PipeWire-server restart need testing. The CLAP host still needs production-plugin compatibility, parameter/event/state support and correct lifecycle-thread integration; VST2/VST3 remain absent. The Qt editor is an initial daemon client, not the complete upstream editor. Highest-value next steps: improve plugin host behavior/failure isolation and expand PipeWire recovery/device-layout tests.
