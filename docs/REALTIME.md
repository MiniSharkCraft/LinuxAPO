# Realtime milestone verification

Fresh real-hardware retest after the unexpected reboot (2026-10-01, PipeWire
1.6.9): selected `alsa_input.pci-0000_00_1f.3.analog-stereo`, capture node 54;
the daemon exposed `SkyAPO Virtual Mic` (`skyapo.virtual_mic`, node 93) at F32
planar, 48000 Hz, stereo FL/FR, quantum 1024. A second PipeWire client recorded
the physical source and virtual source concurrently for 192000 frames each.
Measured correlation was 1.0, alignment lag 0 frames, RMS ratio 0.501187 and
gain −6.000 dB for `Preamp: -6 dB`. Live status reported 0 callback
allocations/deallocations and 0 overruns (average/max processing time 68.34 /
162.82 μs). The recorder's raw and processed WAVs remain in the ignored build
directory as `proof-final-20261001-1630-raw.wav` and
`proof-final-20261001-1630-processed.wav`; they contain microphone audio. The
daemon received SIGTERM and removed its virtual source afterward.

Fresh physical-device verification on 2026-10-14 (PipeWire 1.6.9): with the
user daemon and service inactive, `skyapo device list/current` selected
`alsa_input.pci-0000_00_1f.3.analog-stereo` (Built-in Audio Analog Stereo,
node 54). A foreground `skyapod --config examples/preamp.txt` negotiated F32
planar DSP, 48 kHz, stereo FL/FR, quantum 1024, and created `SkyAPO Virtual
Mic` (`skyapo.virtual_mic`, node 84). `wpctl status` and `pw-dump` both showed
the source. `skyapo-realtime-probe` explicitly captured the physical input and
virtual output concurrently with fallback disabled: 192000 frames each,
correlation 1, alignment lag 1024 frames, RMS ratio 0.501187 and gain −6 dB.
During capture `skyapo status --verbose` reported input RMS 0.721468, output
RMS 0.36159, DSP ratio 0.501187, one upstream Preamp filter, 2/2 active links,
60.8055 μs average / 112.126 μs maximum processing time, 0 overruns, and 0
audited callback allocations/deallocations. The foreground daemon exited
cleanly on SIGINT; subsequent status reported unreachable and PipeWire no
longer listed the virtual source. The configured default source remained
`music_virtual_mic`. Temporary raw/processed microphone WAVs were deleted
after numerical verification. This validates a live physical capture/DSP/
virtual-source round trip on the current device, not unplug/replug, other
hardware, or calibrated end-to-end latency.

Repeat physical-device verification on 2026-10-14 after response-analysis
changes: PipeWire 1.6.9 was active, with the user service still disabled and
inactive. The selected source remained
`alsa_input.pci-0000_00_1f.3.analog-stereo` (capture node 54). A foreground
`build/skyapod --config examples/preamp.txt` exported `SkyAPO Virtual Mic`
(`skyapo.virtual_mic`, node 83); both `wpctl status` and `pw-dump` showed the
source and active FL/FR capture links. Negotiation was F32 planar DSP, 48 kHz,
stereo FL/FR, quantum 1024. The independent native
`build/skyapo-realtime-probe` recorded 192000 frames each from the physical
input and virtual source with fallback disabled: correlation 1, alignment lag
1024 frames, RMS ratio 0.501187 and gain −6 dB. Live status during streaming
showed input RMS 0.522735, output RMS 0.261988, DSP ratio 0.501187, 2/2
capture links, 0 overruns and 0 audited callback allocations/deallocations
(processing average/max 67.9488/259.954 μs). The foreground daemon exited 0 on
SIGINT and its virtual source disappeared; subsequent status was unreachable.
`skyapo config show` remained `Preamp: 0 dB`, the selected physical node was
unchanged, and `pw-metadata -n default` still showed configured source
`music_virtual_mic` (WirePlumber's current `default.audio.source` metadata
resolved to the physical node). The temporary raw/processed microphone WAVs
were deleted. This proves live processing and independent consumption on this
workstation device, not physical unplug/replug or total application latency.

Current workstation revalidation on 2026-10-01: the selected physical source
was `alsa_input.pci-0000_00_1f.3.analog-stereo` (Built-in Audio Analog
Stereo, PipeWire node 88). SkyAPO created `SkyAPO Virtual Mic`
(`skyapo.virtual_mic`, node 118) with F32 planar, 48000 Hz, stereo FL/FR,
quantum 1024 and two active capture links. An independent two-stream recorder
captured 192000 frames from the physical and virtual sources: correlation 1,
alignment −1024 frames and RMS ratio 0.501187 (−6 dB), matching
`10^(-6/20)`. `skyapo status` reported one upstream Preamp filter, zero
callback allocations/deallocations and zero overruns. The daemon was stopped,
the temporary XDG device selection was isolated from the user's config, and
the raw/processed microphone WAV files were deleted after measurement. Node
IDs are specific to this PipeWire session; the persisted selection is the
stable `node.name`.

Fresh physical Convolution check on 2026-10-15 (PipeWire 1.6.9): with an
isolated `XDG_CONFIG_HOME` and foreground daemon, the selected built-in capture
device was `alsa_input.pci-0000_00_1f.3.analog-stereo` (node 54). A mono, 48 kHz
PCM16 impulse response containing one sample at 0.5 was applied by the actual
upstream Convolution filter. The daemon exposed `SkyAPO Virtual Mic`
(`skyapo.virtual_mic`, node 70) at F32 planar DSP, 48 kHz stereo FL/FR, quantum
1024. After the graph was ready, status showed one Convolution filter, 0
overruns, and 0 audited callback allocations/deallocations (55.56 μs average,
158.99 μs maximum in the captured interval). `skyapo-realtime-probe` explicitly
recorded 192000 frames from the physical input and virtual source with fallback
disabled; correlation was 1.0, alignment lag 1024 frames, and RMS ratio was
0.5 (−6.0206 dB), matching the single-tap IR. `wpctl status` showed both the
built-in device and virtual mic. Temporary config, impulse, and microphone WAVs
were automatically removed; the daemon exited and removed its virtual source.
This closes physical same-rate convolution verification on this machine only;
it does not test alternate hardware rates, multichannel IRs, physical hotplug,
or total end-to-end latency.

Current-worktree physical Preamp revalidation on 2026-10-15 (PipeWire 1.6.9):
using isolated `XDG_CONFIG_HOME`, foreground `build/skyapod --config
examples/preamp.txt`, and the actual selected source
`alsa_input.pci-0000_00_1f.3.analog-stereo` (node 54), `wpctl status` and
`pw-dump` both showed `SkyAPO Virtual Mic` (`skyapo.virtual_mic`, node 70).
The negotiated graph was F32 planar, 48 kHz stereo FL/FR, quantum 1024, with
2/2 capture links. An independent native PipeWire recorder explicitly
captured the physical and virtual sources with fallback disabled for 192000
frames each. Correlation was 1.0, alignment lag 0 frames, RMS ratio 0.501187,
and gain −6 dB for the actual upstream `Preamp: -6 dB` filter. Streaming
status showed one filter, input/output RMS 0.0283227/0.014195, 13.975/24.095
μs average/maximum processing time, zero overruns and zero audited callback
allocations/deallocations. The daemon exited cleanly, the virtual source
disappeared, and the two temporary microphone WAVs were unlinked after
measurement. This revalidates current source/runtime on this physical device;
it does not cover device unplug/replug, other hardware, or calibrated
end-to-end latency.

A fast status poll immediately after launching a foreground daemon on this
physical source also caught the startup race before graph negotiation: PipeWire
reported `Daemon: streaming` while SkyAPO reported
`DSP state: initializing (awaiting negotiated format)`,
`Filters: unknown (DSP engine not initialized)`, and an uninitialized filter
chain. The next poll showed 48 kHz and the configured Preamp. The daemon was
stopped cleanly and removed its virtual source. This verifies truthful
transient status on this host; it is not an audio-capture or recovery test. The
same immediate-state assertion passed with the ASan/UBSan daemon
(`ASAN_OPTIONS=detect_leaks=0` for the known external PipeWire context leak),
which then reached the same negotiated 48 kHz/one-filter state without memory
access or undefined-behavior diagnostics.

Verified 2026-09-30 on the existing Arch user PipeWire 1.6.8 / WirePlumber session. No GUI/plugin work was undertaken.

## Actual chain

Selected physical source `alsa_input.pci-0000_00_1f.3.analog-stereo`, description **Built-in Audio Analog Stereo**, node 88, output ports 97/98 (FL/FR). Stable node name is persisted in the XDG config directory; numeric IDs below describe only this session.

The final normal daemon exported `skyapo.virtual_mic`, description **SkyAPO Virtual Mic**, node 174, media class Audio/Source. Capture input ports were 114/43. `pw-dump` observed active links 45/34 from physical node 88 to SkyAPO 174, and active links 134/154 from SkyAPO 174 to recording client node 124. The parallel raw-reference recording client was node 116. `wpctl status` listed the virtual source at unity volume.

Format: native planar float32 DSP ports, 48000 Hz, stereo FL/FR. Recording clients negotiated interleaved float32 48000 Hz stereo via normal PipeWire conversion. Observed quantum: 1024 frames (21.333 ms of graph time, **not** an end-to-end latency measurement). One actual upstream Preamp filter, config `examples/preamp.txt`, -6 dB. No additional latency estimate is claimed.

Two independent native recording streams explicitly target physical and virtual sources with fallback disabled. Each collected 192000 frames. Final normal and instrumented recordings both produced correlation 1, RMS amplitude ratio **0.501187**, gain **-6 dB**. Alignment lag was 0 frames in the final normal recording and 1024 frames in the instrumented recording. Lag is measured between these recording streams, not a calibrated hardware latency. WAVs are under ignored `build/*-raw.wav` / `build/*-processed.wav` and contain microphone audio.

An earlier implementation was visible but failed WirePlumber format negotiation; a client could fall back to the physical source, producing ratio 1. This was not accepted as success. The source now advertises Format/EnumFormat and acknowledges PortConfig transitions, and the test refuses fallback. Real consumption and numerical DSP gain were verified afterward.

The debounced inotify config watcher was subsequently tested live. The daemon loaded `tests/data/reload_minus6.txt`, then a replacement with `Preamp: -3 dB`; status reset per-chain meters and reported ratio 0.707946. An independent client then captured 192000 frames from the virtual source: correlation 1 and ratio 0.707946. Replacing the config with `UnsupportedDirective: true` produced a source/line reload error; status stayed streaming and an independent recording still measured ratio 0.707946. Callback allocation/deallocation counts remained zero. This verifies valid reload, bad reload rollback and real consumption of the retained graph.

The private automated `skyapo-pipewire-e2e-include-reload` issues five successive valid reload requests, checks invalid-config rollback, removes/recreates an active Include directory, and now also changes the root to a not-yet-created Include. The daemon keeps the last valid −2 dB graph while reporting the missing file, then automatically watches/reloads the new Include when it is created; status reports the new −4 dB chain and the independent virtual-source recording matches. `skyapo-config-watcher` also covers inotify queue overflow and discovery of a not-yet-created nested directory/file. These tests stress last-good retention and recovery, but do not claim to force a specific control request into an exact callback quantum of the 10 ms transition.

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
build/skyapo device set alsa_input.pci-0000_00_1f.3.analog-stereo
build/skyapo device current
build/skyapod --config examples/preamp.txt
build/skyapo status
wpctl status
pw-dump
build/skyapo-realtime-probe alsa_input.pci-0000_00_1f.3.analog-stereo build/final-proof [-6]
build/skyapo-realtime-probe alsa_input.pci-0000_00_1f.3.analog-stereo build/reload-proof -3
```

Both builds succeeded; both CTest suites passed **3/3** (core, realtime allocation safety, offline WAV). After adding Channel/Copy, the latest normal CTest was 1.25 s and ASan/UBSan CTest 3.48 s. Core tests include actual upstream Preamp/BiQuad/IIR/Delay/Channel/Copy, nested Include, errors and varying blocks. Unsupported commands remain errors, not silently skipped.

### Automated private-server end-to-end regression

`SKYAPO_ENABLE_PIPEWIRE_E2E_TESTS=ON` adds mono and stereo private-graph tests to CTest; each launches a private PipeWire server under temporary `XDG_RUNTIME_DIR`/`XDG_CONFIG_HOME`, enumerates/selects a deterministic source, checks live daemon status, and lets an independent native PipeWire consumer link to and record `skyapo.virtual_mic`. Both modes assert F32/48 kHz, quantum 1024, negotiated channel/link count, -6 dB DSP ratio and zero audited callback allocations/deallocations. Before recording, the test uses runtime IDs from status plus `pw-dump` to destroy one physical-to-SkyAPO link with `pw-cli`, waits for `skyapod` to restore the complete link count without exiting, then verifies the independent recording numerically. A source-replug mode stops the selected deterministic source, verifies the daemon stays alive while the status socket is unavailable, republishes the same stable `node.name`, and checks the `SkyAPO Virtual Mic` node and all capture links return before an independent client verifies -6 dB output. A server-restart mode stops the private PipeWire server and source, restarts both, then verifies daemon reconnection, the virtual-source properties, capture links and recorded -6 dB result. PipeWire runtime IDs may be reused after a full server restart; the test validates node properties and audio graph rather than assuming IDs are globally unique. On 2026-09-30 mono/stereo link-loss, synthetic source-replug and private server-restart tests passed. These private-graph tests are repeatable without physical hardware and do not validate physical device unplug/replug. Hosted Linux CI run [38513757741](https://github.com/MiniSharkCraft/LinuxAPO/actions/runs/38513757741) later passed on commit `e1add4c`; the current synchronized worktree has additional local verification recorded above and in `CHANGELOG.md`.

On 2026-10-14, `skyapo-pipewire-e2e-source-consumer-latency` added a bounded stereo timestamp probe through the private graph. The synthetic producer encodes `CLOCK_MONOTONIC` timestamps in 17–18 low-amplitude impulses; an independent PipeWire consumer decodes them after the live daemon's upstream `Preamp: -6 dB` path and measures producer-callback to consumer-callback wall delay. Three serial repetitions passed at 48 kHz / 1024 frames: median 116–178 µs and p95 182–221 µs. This boundary is a useful measurement of the synthetic graph's processing handoff, not sample age/quantum buffering as seen by an application, physical ADC/device latency, or total end-to-end latency. A prior draft used shared PipeWire graph frame positions and returned 0 frames/correlation 1.0; that only established a common logical timeline and is not treated as latency evidence.

Playback server-restart coverage was added on 2026-10-17. The dedicated
`skyapo-pipewire-e2e-playback-wireplumber-server-restart` test stops and
recreates the private PipeWire server, WirePlumber policy process, selected
SPA null sink, and deterministic capture source. It waits for both playback
link pairs and capture links to recover, then launches an ordinary playback
test stream and independently records the sink monitor. Release and
ASan/UBSan runs passed **1/1** each. Release measured a post-restart
sink-monitor output/expected RMS ratio of **0.991431** (within the 3%
tolerance); daemon status reported `Preamp: -6 dB`, capture links 2/2,
playback internal/target links 2/2, zero audited callback
allocations/deallocations, and zero overruns. This is synthetic private-graph
recovery evidence, not physical sink restart/hotplug validation or proof of
uninterrupted audio during server downtime.

The private E2E matrix now forces and verifies 44100, 48000 and 96000 Hz independently for both mono and stereo. The helper fails closed if the installed base `pipewire.conf` no longer has exactly one commented setting for each forced clock property. Both daemon status and an independent consumer's negotiated graph clock are checked, along with `MONO` or `FL FR`, F32 planar DSP, 1024-frame quantum, capture-link count and numerical -6 dB output. Local regression on 2026-10-01 passed all six cases; RMS ratio to expected processed output ranged from 1.000004 to 1.000031, with zero audited callback allocations/deallocations and zero overruns. The default full PipeWire-enabled CTest suite passed 58/58; the opt-in FST/VST2 suite passed 57/57, including a 32→64-sample VST2 latency notification, updated PDC/bypass impulse assertions and a private PipeWire E2E with an independent consumer. The ASan/UBSan/LeakSanitizer suite passed 35/35; PipeWire E2E remains disabled in that build. The E2E suites use an isolated synthetic PipeWire source and consumer, not the workstation microphone; hardware-specific claims above remain separate.

The E2E harness now also runs independent 5.1 and 7.1 source → SkyAPO → consumer graphs. On this source revision both tests passed at F32P, 48 kHz, quantum 1024. The 5.1 graph negotiated `FL FR FC LFE RL RR`, 6/6 capture links, 143360 consumer frames, expected-output RMS ratio `1.000017`, zero callback allocations/deallocations and zero overruns. The 7.1 case independently checks `FL FR FC LFE RL RR SL SR` and all 8 links; both use the deterministic identical-channel signal and `Preamp: -6 dB`. Together with the mono E2E, these exercise all nine channel labels accepted by the current runtime. Both tests also pass under ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0`; LeakSanitizer is disabled for this integration run because PipeWire context/library allocations are independently known to persist at teardown. The full default CTest run passed 64/64 and the sanitizer suite 35/35. This is PipeWire graph/mapping evidence, not a physical multichannel microphone test or exhaustive real-device channel-mask proof.

Realtime allocation tests first verify allocator hooks detect deliberate allocations, then process 1000 variable-size blocks each in mono and stereo, plus 1000 blocks through Channel/Copy/Preamp/BiQuad/Delay routing: zero callback allocations/deallocations. Live status likewise reported **0 allocations / 0 deallocations**, and zero processing overruns in sampled normal/instrumented runs. Counters cover executable C++ and wrapped C allocation paths (including linked EAPO code), **not shared-library C allocator internals**. Buffers/pointer arrays/DSP states are initialized outside the callback. Process timing uses lock-free atomics and monotonic timestamps; logging/status serialization runs in the main loop.

Singleton startup was tested: a second daemon exits with an explicit lock error. Destroying only the SkyAPO node with `pw-cli destroy 170` caused reconnect after two seconds and recreated source node 174 with physical links. SIGINT teardown removed the source/socket; subsequent status explicitly reported `Daemon: not reachable`. The physical device/default source was not removed or rerouted. Full PipeWire-server restart and physical device unplug were not exercised.

### Sanitizer caveat

Unit/offline/allocation CTest ran with leak checking enabled and passed. Real PipeWire daemon/probe teardown triggers LeakSanitizer reports in installed module/context initialization. The standalone `skyapo-pipewire-lifetime` diagnostic, containing only PipeWire init/context/connect/disconnect/destroy/deinit and no SkyAPO/EAPO code, reproduces **3677 bytes / 34 allocations**. Daemon teardown reported 7354 bytes / 68 allocations (two contexts); the two-stream probe reported 3709 bytes / 34 allocations. This is an observed dependency-lifetime issue, not a clean realtime LeakSanitizer pass.

The realtime daemon, live graph reload, and recording test were subsequently run with `ASAN_OPTIONS=detect_leaks=0`; address/undefined behavior instrumentation remained enabled, the reloaded recording passed at -3 dB, and shutdown exited 0 without ASan/UBSan memory-access diagnostics. No blanket suppression was added to project builds or CTest. Investigate the installed PipeWire modules' leaks separately. For the latest sanitizer regression run, `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure` passed **24/24**; this verifies address/undefined behavior instrumentation but, by explicit option, does not verify leak freedom in that run.

### Current realtime revalidation

On 2026-09-30 the selected source was `alsa_input.pci-0000_00_1f.3.analog-stereo`, runtime node 88 (Built-in Audio Analog Stereo). PipeWire negotiated F32 planar DSP, 48 kHz, stereo FL/FR, quantum 1024. `SkyAPO Virtual Mic` appeared as `Audio/Source`, node 174. `skyapo-realtime-probe` captured 192000 frames from both the physical device and virtual source: lag 1024 frames, correlation 1, and measured amplitude ratio 0.501187 for the `Preamp: -6 dB` example. The daemon reported 0 callback allocations, 0 callback deallocations, and 0 overruns. The same two-client capture succeeded with the ASan/UBSan daemon (`detect_leaks=0` for the known external PipeWire context leak).

Fresh revalidation after the Qt/device-selector and config-parser changes used a foreground daemon with `tests/data/reload_minus6.txt` (the user config was not changed). `wpctl status` showed physical node 88 and `SkyAPO Virtual Mic` node 174 simultaneously; `pw-dump` showed active physical-to-SkyAPO links 114 and 45 (FL/FR). Both no-fallback native recorder streams negotiated interleaved F32, 48000 Hz, stereo and captured 192000 frames. Measured correlation was 1, alignment lag −1024 frames, and RMS ratio 0.501187 (−6 dB). During the run, `skyapo status` reported quantum 1024, one active Preamp filter, 2/2 capture links, zero overruns, and zero audited callback allocations/deallocations. The daemon stopped cleanly and removed its virtual source; generated microphone WAVs were deleted after measurement. The lag is recorder alignment, not a calibrated latency value.

Latest physical-device rerun on the workstation used the stable source `alsa_input.pci-0000_00_1f.3.analog-stereo` (runtime node 88) and created `SkyAPO Virtual Mic` (`skyapo.virtual_mic`, runtime node 222). PipeWire 1.6.8 negotiated F32 planar, 48 kHz stereo FL/FR, quantum 512; `pw-link -l` showed both physical FL/FR ports connected to SkyAPO. With `examples/preamp.txt`, status measured input RMS 0.025043, output RMS 0.0125513, DSP ratio 0.501187, zero callback allocations/deallocations and zero overruns. The independent native recorder captured 192000 raw and virtual frames, correlation 1, alignment −1024 frames, RMS ratio 0.501187 and gain −6 dB. `skyapo stop` removed the virtual source cleanly. Separately, the core test measured the actual upstream `LoudnessCorrectionFilter` 100 Hz RMS ratio 0.777543 using a supplied +12 dB endpoint snapshot; that measurement is from the DSP test, not hardware LoudnessCorrection recording.

On 2026-10-01, rechecked the currently running desktop chain without changing the user's config or selected device: physical `alsa_input.pci-0000_00_1f.3.analog-stereo` node 88 → daemon → `SkyAPO Virtual Mic` node 118. Negotiated format was F32 planar, 48 kHz stereo FL/FR at quantum 1024. `skyapo-realtime-probe` connected simultaneous physical and virtual clients and recorded 192000 frames from each; at the currently configured 0 dB Preamp it measured correlation 1, alignment 0 frames, RMS ratio 1 and gain 0 dB. Post-probe status remained streaming with 2/2 links, zero overruns and zero audited callback allocations/deallocations. Temporary WAV files were removed. This is a live pass-through chain check; the separately documented −6 dB hardware run and private E2E remain the numerical attenuation evidence.

The same independent two-client hardware test was run with the ASan/UBSan daemon and probe using `ASAN_OPTIONS=detect_leaks=0` (LeakSanitizer only). Both streams captured 192000 frames at F32/48 kHz/stereo; correlation 1, lag 1024 frames, and RMS ratio 0.501187 (−6 dB). `pw-dump` observed active physical-to-virtual links 170 and 114; status reported 2/2 links, zero overruns, and zero audited callback allocations/deallocations. AddressSanitizer and UBSan remained enabled and emitted no memory-access/undefined-behavior diagnostics; the daemon removed the source on clean stop. Leak detection is disabled only for this real PipeWire process because the external PipeWire context/module leak is independently reproducible; normal sanitizer CTest retains leak checking.

### Virtual-source loss recovery

On 2026-09-30, a separate `skyapod` instance used an isolated `XDG_CONFIG_HOME` and the selected stable device name `alsa_input.pci-0000_00_1f.3.analog-stereo` (runtime physical node 88). At F32 planar, 48 kHz, stereo FL/FR, quantum 1024, `wpctl status` showed `SkyAPO Virtual Mic` node 174 and the daemon reported 2/2 capture links. Destroying only node 174 with `pw-cli destroy 174` caused the daemon to report `virtual source disconnected`, exit that runtime instance and retry; within about three seconds node 116 appeared with the daemon back in `streaming`, 2/2 links and zero overruns/host callback allocations/deallocations. An independent `skyapo-realtime-probe` then recorded 192000 frames from both the physical source and recreated virtual source, measured correlation 1, RMS ratio 0.501187 (−6 dB), and alignment lag −1024 frames. The daemon was stopped cleanly and the generated microphone WAV files were removed. This proves virtual-node-loss recovery, not a PipeWire server restart or physical-device hotplug test.

### PipeWire server restart recovery

On 2026-09-30, an isolated PipeWire 1.6.9 server used the non-installed `skyapo-pipewire-test-source` fixture, which publishes a deterministic 440 Hz stereo source under the stable name `skyapo.test.input`. SkyAPO selected that name and processed it at F32 planar, 48 kHz, stereo FL/FR, quantum 1024 with `Preamp: -6 dB`; before restart status measured RMS ratio 0.501187, with 2/2 links, zero overruns and zero audited callback allocations/deallocations. Stopping only this private PipeWire server produced `connection error`; `skyapod` removed the dead runtime and retried while the test source exited. After restarting the private server and test source with the same node name, SkyAPO rediscovered runtime node 33, recreated `SkyAPO Virtual Mic` as node 37, restored both FL/FR links, and returned to `streaming`. Status again measured RMS ratio 0.501187 and zero overruns/host callback allocations/deallocations. `pw-link -l` confirmed both active input links. The independent consumer verification after restart is recorded below. Physical desktop audio/server state was not changed.

The fixtures can be built with `cmake --build build --target skyapo-pipewire-test-source skyapo-pipewire-test-consumer`; they are test-only and are not installed in the product package. Run the source with no arguments for stereo or `skyapo-pipewire-test-source --mono` for its stable mono test source. Run the consumer with no arguments for stereo or `skyapo-pipewire-test-consumer --mono` for a mono virtual mic.

### DSP callback exception containment

Unexpected C++ exceptions from `Engine::process()` are caught inside the
PipeWire process callback. SkyAPO zeros the current output buffer, latches the
DSP fault, and signals the control loop; the daemon and virtual source remain
available while audio stays silent. A valid config reload drains/discards an
interrupted graph transition, builds a fresh Engine off-thread, then resumes
processing. A failed reload leaves the graph faulted and silent. Two private
PipeWire E2Es verify an exception at callback entry and an exception after a
crossfade has processed at least one block; an independent consumer checks
silence, invalid-reload retention, and recovery to the expected −6 dB output.
This exception path may allocate for C++ exception unwinding, so zero-allocation
claims apply to normal processing rather than exception handling. This is not
native crash/hang isolation or protection against memory corruption.

### Live capture-device selection

The private `skyapo-pipewire-e2e-device-switch` test starts with `skyapo.test.input`, starts a second independent source named `skyapo.test.alternate`, then runs `skyapo device set skyapo.test.alternate` while `skyapod` is streaming. On the 2026-10-01 run, the runtime rebuilt from capture node 33 to node 45, reported `Selected device: skyapo.test.alternate`, restored 2/2 FL/FR capture links, and kept `SkyAPO Virtual Mic` visible. An independent native PipeWire consumer captured 144384 frames at 48 kHz stereo; measured RMS ratio was 1.000004 against expected output for `Preamp: -6 dB`. Status reported zero overruns and zero audited callback allocations/deallocations. These IDs are private-graph runtime IDs only. This verifies switching between two available stable node names; it does not verify unplug/replug of physical hardware or automatic client policy/default-source selection.

### Plugin process failure and virtual-source consumption

On 2026-09-30, a private PipeWire graph used `skyapo.test.input` node 33, `SkyAPO Virtual Mic` node 37, F32 planar, 48 kHz stereo FL/FR and quantum 1024. The config loaded the test CLAP plugin `org.skyapo.test.error-once`, which returns `CLAP_PROCESS_ERROR` on its first callback. `skyapo status` then reported `Plugin failures: org.skyapo.test.error-once — Plugin — .../config.txt:1`, remained `streaming` with 2/2 capture links, and showed output RMS 0, zero overruns, and zero audited callback allocations/deallocations. `pw-dump` showed `skyapo.virtual_mic`. The independent native PipeWire consumer explicitly linked to the virtual source and captured 141312 stereo frames at RMS `0.00000000`; it exited 0 with `--expect-silent`. This confirms the failed node becomes silence while the PipeWire source remains consumable; it is not a third-party plugin crash/hang isolation test. The private source/server/daemon were stopped afterward and the desktop server was untouched.

On 2026-10-14, `skyapo-pipewire-e2e-vst3-exception-failure` loaded a fixture
whose VST3 `IAudioProcessor::process()` throws a C++ exception. SkyAPO caught
it within the `noexcept` adapter, latched the plugin failed, and an independent
consumer recorded silence while the daemon stayed streaming and the virtual
source remained available. Status retained the failing VST3 ID. The realtime
allocation audit observed exactly one allocation and one deallocation during
the exception path; this is not an allocation-free failure path. Ordinary
processing and reported API-error paths remain zero-allocation. Release CTest
passed 70/70 and ASan/UBSan with LeakSanitizer enabled passed 71/71 including
this test. This does not contain native crashes, `abort()`, memory corruption
or hangs; those still require process isolation.

### Mono layout and selected-source loss

The same fixture's `--mono` mode publishes one `MONO` channel under the stable name `skyapo.test.mono`. On a separate private PipeWire server, `skyapo device list` reported one channel; after `skyapo device set skyapo.test.mono`, `skyapod` exposed a mono `SkyAPO Virtual Mic` and linked `capture_MONO` to `input_MONO`. At 48 kHz / quantum 1024, status measured RMS ratio 0.501187 for `Preamp: -6 dB`, with 1/1 links, zero overruns and zero audited callback allocations/deallocations. Stopping the fixture removed the selected source; the daemon reported `selected capture device disappeared` and retried. Restarting the fixture with the same stable node name led it to rediscover the source, rebuild the mono virtual node and restore the single link; status again measured 0.501187. An independent native PipeWire consumer captured 143360 mono frames from the virtual mic and measured RMS ratio 0.999974 against the expected −6 dB output. After a private PipeWire server restart it captured a further 144384 frames and measured 0.999999. This validates mono processing and synthetic selected-source disappearance/reappearance, not physical USB/ALSA unplugging.

### PipeWire consumer churn

The non-installed `skyapo-pipewire-test-consumer` client explicitly links its input ports to the virtual source and accumulates samples in its PipeWire process callback; it does not need WirePlumber to create the test links. In stereo it completed two independent 144384-frame captures across separate client lifetimes, measuring expected-output RMS ratios 0.999993 and 1.000003 (expected absolute RMS 0.03543929 for the deterministic 0.1-amplitude sine after −6 dB). After the consumer exited, the daemon remained `streaming` with 2/2 capture links, zero overruns and zero audited callback allocations/deallocations. Mono captures are recorded above. This verifies direct native PipeWire client consumption and attach/detach/reattach, not automatic desktop policy selection.

### Live sample-rate and quantum renegotiation

With the same isolated PipeWire 1.6.9 server, deterministic stereo source, native consumer and `Preamp: -6 dB`, SkyAPO started at 48 kHz / quantum 1024. While `skyapod` remained running, setting the private server's `clock.force-quantum` to 512 caused Engine reinitialization at 48 kHz / 512; the consumer captured 143872 frames and measured RMS ratio 0.999980 against the expected processed output. Setting `clock.force-rate` to 44100 kept quantum 512; the consumer captured 132096 frames and measured 0.999987. Setting rate 96000 and quantum 2048 caused another rebuild; the consumer captured 286720 frames and measured 0.999970. At each stage daemon status remained `streaming`, reported the negotiated rate/quantum, 2/2 input links, zero overruns and zero audited callback allocations/deallocations; upstream DSP amplitude telemetry remained 0.501187 relative to the input. The private server was stopped afterward, so no desktop PipeWire settings needed restoring. These are live renegotiation tests with Preamp, not a claim that fixed-block Convolution was exercised through a quantum change.

This live-reconfiguration path is now an automated `skyapo-pipewire-e2e-renegotiate` test. On 2026-10-01 it forced 48 kHz/512, 44.1 kHz/512, then 96 kHz/2048 while the daemon stayed running; independent recordings measured expected-output RMS ratios 1.000006, 0.999991, and 1.000039. It then removed and recovered a physical-to-virtual test link and captured again at 96 kHz (ratio 1.000007). This automates sample-rate/quantum changes between stable graphs; the following E2E covers a format change during a pending config crossfade.

Runtime status now compares the active Engine's sample rate and `maxFrames()`
against PipeWire's latest negotiated rate and quantum. During the existing
fail-silent format-mismatch path it reports `DSP state: reconfiguring` rather
than `running`; the Qt status summary identifies the output-silent rebuild.
The Release and ASan/UBSan renegotiation E2Es still reach each target format and
independent recording successfully. Those E2Es validate recovery/audio after
rebuild, but do not guarantee they sample the short intermediate status window.

The separate `skyapo-pipewire-e2e-transition-format` case compiles an E2E-only transition-duration override (500 ms; normal builds stay at 10 ms), waits until `Graph transition: crossfading` is observable, then changes the private server to 44.1 kHz. Runtime reported `Format rebuilds during transition: 1`, settled at 44.1 kHz / quantum 512 with 2/2 links and zero overruns/audited callback allocations, and the independent consumer measured ratio 1.000027 to the accepted −3 dB graph. This directly exercises the pending-graph format-rebuild branch; the longer duration exists only to make that scheduling window deterministic in E2E.

### Explicit daemon process restart

On 2026-09-30, `skyapo start` and `skyapo restart` were exercised against the live desktop PipeWire server with isolated XDG configuration; separate PIDs `1726284` and `1726296` both returned to streaming with 2/2 links. The stronger acceptance used an isolated PipeWire 1.6.9 graph and deterministic `skyapo.test.input` source (node 33), with `Preamp: -6 dB`. Before restart, daemon PID `1771944` exposed `SkyAPO Virtual Mic` node 37 at F32 planar, 48 kHz, stereo FL/FR, quantum 1024; status measured amplitude ratio 0.501187, zero overruns and zero audited callback allocations/deallocations. `skyapo restart` created PID `1772247`, restored the same virtual source/format/links/filter and 0.501187 ratio. The independent native consumer then exited 0 after capturing 144384 stereo frames: RMS 0.03543952 versus expected 0.03543929, ratio 1.000006. Status remained streaming with 2/2 links and zero overruns/allocations after the consumer detached; `skyapo stop` and private server shutdown were clean. A separate physical-mic probe during changing ambient input produced correlation 0.199082 and was discarded; it is not used as gain evidence.

### User-systemd daemon crash recovery

On 2026-10-14, the opt-in `skyapo-pipewire-e2e-systemd-crash-restart` test ran against the current user's active systemd manager and a private PipeWire graph/source. It reads `Restart`/`RestartSec` from `packaging/skyapod.service.in`, launches a uniquely named transient user service with those settings and isolated XDG/PipeWire paths, then deliberately SIGKILLs only that test unit's `skyapod`. systemd reported one restart and replaced PID `404840` with `404926`; the daemon returned to `streaming` with its selected test source and `SkyAPO Virtual Mic` visible, F32 planar 48 kHz stereo FL/FR, quantum 1024, 2/2 links, zero overruns and zero audited callback allocations/deallocations. An independent PipeWire consumer recorded 143360 frames and measured RMS ratio `0.999961` against the expected `Preamp: -6 dB` output. PipeWire reused virtual node ID 37 after the service restart, so node-ID change is not used as evidence; the test verifies the node's properties and independent audio instead. This tests recovery when users run SkyAPO under systemd; manual `skyapod` execution is not supervised, and it does not establish recovery from an in-process plugin crash (which may terminate or corrupt the daemon before systemd restarts it).

The test is opt-in because it requires a functioning user systemd manager and intentionally kills a daemon process: configure with `-DSKYAPO_ENABLE_PIPEWIRE_E2E_TESTS=ON -DSKYAPO_ENABLE_SYSTEMD_E2E_TESTS=ON`, build, then run `ctest --test-dir build --output-on-failure -R '^skyapo-pipewire-e2e-systemd-crash-restart$'`. It uses a transient test-only unit and does not start/stop the installed `skyapod.service`.

Playback crash recovery was added on 2026-10-17 as the separate opt-in
`skyapo-pipewire-e2e-playback-wireplumber-systemd-crash-restart` test. It uses
the same transient-unit policy, a private PipeWire/WirePlumber graph and SPA
null sink; after SIGKILL it requires a new daemon PID, the playback internal
and sink links (2/2 each), and a fresh ordinary playback stream recorded
independently from the sink monitor. Release CTest passed **1/1** and the
ASan/UBSan test passed **1/1** (`detect_leaks=0` for this PipeWire integration
process). The Release monitor measured an output/expected RMS ratio of
**0.991431** for `Preamp: -6 dB`; status reported capture links 2/2, zero
audited callback allocations/deallocations and zero overruns. The related
capture-systemd crash test and playback PipeWire-server-restart test also
passed in the same Release run (**3/3** total). This verifies fresh playback
after supervised process recovery, not uninterrupted audio across SIGKILL or
physical hardware behavior. Run it with the same CMake option, selecting the
new test name in CTest.

### Sustained-load overrun reporting

On 2026-09-30, an isolated PipeWire 1.6.9 graph used deterministic source `skyapo.test.input` (node 33) and virtual source node 37 at F32 planar, 48 kHz, stereo FL/FR, quantum 1024 (21.33 ms callback budget). Baseline `Preamp: -6 dB` had 2/2 links, zero overruns and zero audited callback allocations/deallocations. A validated hot reload replaced it with 2048 upstream BiQuad `Filter` directives. Over a 20-second observation, block count rose from 607 to 1015 and overruns from 607 to 1015: 408/408 callbacks exceeded budget. Reported process average was 51–53 ms and maximum 338.093 ms; status still showed `streaming`, 2/2 links and zero host-audited allocations/deallocations. This deliberately overloaded setup proves the overrun counter/telemetry responds; it does not establish usable realtime audio under overload, nor does the allocation audit cover allocations inside shared-library plugin code. The private daemon/source/server were stopped, and the desktop PipeWire session remained intact.

The actual upstream Convolution and GraphicEQ implementations execute in offline core tests. A separate allocation audit executes 1000 fixed-size upstream Convolution blocks with zero audited callback allocations/deallocations. Convolution is fixed-block: Engine rejects variable callback blocks; the daemon delays graph construction until negotiated quantum and handles rate/quantum changes on the control loop. Physical same-rate convolution is now verified in the 2026-10-15 workstation test above. Hardware tests of convolution at alternate rates/quantums remain outstanding.

### PipeWire playback and default render-volume status

Playback-processing follow-up (2026-10-15): `skyapo device output
list/set/current` enumerates output sinks and persists the selected sink by
stable identity where available. The daemon creates a PipeWire `Audio/Sink`
node named `SkyAPO Playback`, negotiates F32 format, and processes its input
with a separate EAPO-backed engine before linking output ports to the chosen
sink. The direct-link private-graph E2E records `Preamp: -6 dB` at 48 kHz
stereo/Q1024 (RMS ratio 0.999956). A second opt-in E2E starts private
WirePlumber 0.5.18 and PipeWire 1.6.9, creates a real SPA
`support.null-audio-sink` test endpoint, then launches an ordinary untargeted
PipeWire playback stream. WirePlumber routes that stream through `SkyAPO
Playback`; an independent recorder is linked to the selected sink's monitor
ports and captured 144384 frames. The measured RMS ratio was **0.999997** of
the expected -6 dB output. Status showed F32 48 kHz, stereo, Q1024, internal
links 2/2, target links 2/2, zero reported overruns and zero audited callback
allocations/deallocations. This demonstrates policy-based application
routing, DSP and delivery to a sink monitor in the isolated test session.

Playback isolation follow-up (2026-10-16): the same private WirePlumber E2E now
runs a second ordinary PipeWire client explicitly targeted at a separate null
sink while the first client is processed by SkyAPO. It checks the client-to-sink
links, verifies that the active playback-node subgraph is acyclic, and records
both sink monitors independently. The processed path measured an output / −6
dB expected ratio of **0.999998**; the explicitly targeted, unprocessed path
measured a unity ratio of **0.995718**. A sequential rerun on 2026-10-17 passed
1/1 and measured **1.000002** on the processed path and **0.993581** on the
separate unity path. `skyapo-pipewire-e2e-playback-wireplumber` passed both
runs. This proves isolation in the private fixture only; it does not
replace physical-output, desktop-session or hardware feedback validation.

It is not physical-speaker or desktop-session validation. Real ALSA/Bluetooth
outputs, behavior across different WirePlumber policy configurations, device
hot-unplug during playback, and audible feedback/listening remain unverified.
The test fixture's SPA null sink is not evidence of a hardware output. The
v0.10.0 playback milestone remains open pending those checks plus the remaining
playback recovery/configuration regression gates.

Playback target switching/recovery follow-up (2026-10-16): while the daemon is
running, `skyapo device output set <node>` now retargets the paired stream
filter without recreating the smart-filter node or restarting the capture
graph. A private WirePlumber E2E kept an ordinary application stream linked to
`SkyAPO Playback`, switched the child stream to a second null sink, confirmed
both target-channel links and recorded the expected processed signal from the
new sink's monitor. Selected-sink disappearance/reappearance was separately
verified for same-name and stable-identity rename cases. The focused IPC/UI and
playback Release group passed **9/9**; the three switch/recovery E2Es passed
ASan/UBSan **3/3** (`detect_leaks=0` for the PipeWire subprocess harness).
These graph fixtures do not verify physical sink hotplug or switching an
ordinary app stream across real hardware outputs. A stopped daemon only saves
the selected output preference; it does not restart itself.

Focused playback revalidation (2026-10-17): after rebuilding the current dirty
candidate, seven sequential Release E2Es passed (**7/7**, 152.36 seconds):
WirePlumber app routing/isolation, quantum variation, `Stage: post-mix`,
selected-target recovery, live output switching, LV2 latency refresh and
playback PDC recovery. The relevant CMake build passed at `--parallel 2`.
The ASan/UBSan `skyapod` target rebuilt at `--parallel 1`, and the focused
WirePlumber playback E2E passed **1/1** in 34.30 seconds with
`ASAN_OPTIONS=detect_leaks=0` (the known external PipeWire teardown limitation).
These are private PipeWire/WirePlumber graph tests; this retest did not emit
audio through physical speakers or validate ordinary desktop routing on the
active user session. No hardware acceptance is inferred from these results.
An additional sequential sanitizer rerun passed **3/3** (69.73 seconds):
playback isolation (34.46 s), callback exception (10.24 s), and playback PDC
recovery (25.03 s). The PipeWire subprocess tests use the project's existing
LeakSanitizer exclusion; this result is ASan/UBSan evidence, not LSan coverage
for external PipeWire teardown allocations.

Full candidate regression (2026-10-17): the current Release build tree passed
all **92/92 CTest tests** sequentially in 580.13 seconds, including 54 private
PipeWire integration tests, 14 playback tests, 20 plugin tests, and five PDC
tests. The current `skyapod`, PipeWire test consumer, and playback-stream test
targets were rebuilt successfully before the focused PDC run. The same
playback PDC recovery E2E passed **1/1** under the `SKYAPO_SANITIZERS=ON`
build (24.86 seconds); `skyapo-core` and `skyapo-realtime-safety` passed
**2/2** under that build (26.07 seconds). This records working-tree evidence,
not a clean checkout or hosted-CI build. The PipeWire scenarios use private
virtual graphs and do not establish physical-hardware playback or capture.

Repeated output-switch continuity check (2026-10-17): the output-switch E2E
now keeps an independent recorder attached to the stable `SkyAPO Playback`
node while the ordinary test application remains active and the selected sink
makes four live round trips between two WirePlumber null sinks. It checks after
each switch that the playback node and app-to-filter link remain present, both
playback target channels and capture links return to streaming, and callback
allocation/deallocation counters remain zero. The recorder measured RMS ratio
`0.999998` against expected processed output across the switches; a separate
recording from the final selected sink measured `1.000004`. The test passed
three sequential Release runs (34.76, 34.68 and 34.76 seconds). This is
stronger private-graph continuity evidence, not physical sink, audible-level,
or active desktop-session validation.

Playback `SPA_PARAM_Props` controls are applied on both the processing
`Audio/Sink` and paired `Stream/Output/Audio` nodes. Control-loop callbacks
validate and atomically publish `mute`, effective per-channel
`channelVolumes`, or scalar `volume` when per-channel values are absent;
realtime callbacks apply gains without heap allocation. Private WirePlumber
E2Es verified an ordinary untargeted app stream: −6 dB EAPO Preamp plus 0.5
per-channel sink gain and 0.5 scalar stream gain produced the expected −18 dB
at the independent selected-sink monitor recorder; stream mute produced
measured silence. Baseline and control runs reported zero callback
allocations/deallocations and zero overruns. These fixture-backed checks do
not establish every desktop mixer or hardware-volume convention.

On 2026-09-30, the live daemon resolved the `default.audio.sink` metadata to `alsa_output.pci-0000_00_1f.3.analog-stereo`. The monitor enumerates `SPA_PARAM_Props` and reports the RMS of `channelVolumes` plus `mute`; unrelated ALSA device Props from the same enumeration are ignored. During desktop verification `wpctl get-volume @DEFAULT_AUDIO_SINK@` reported `0.50`, PipeWire exposed `volume=1.0` but effective per-channel volumes `0.124989`, and SkyAPO status reported `-18.0626 dB` from the channel gains. This demonstrates why generic `SPA_PROP_volume` alone is not a reliable endpoint master level for this ALSA sink. The monitor separately exposes `uniformChannelGainDb` only when all channel gains are equal and positive; asymmetric or zero channel gains are explicitly unavailable, and mute remains separate. The actual upstream `LoudnessCorrection` filter now consumes this conservative Linux snapshot in daemon mode, but SkyAPO does not claim exact equivalence to Windows endpoint master-volume semantics. Offline renderer/config validation rejects the endpoint-dependent directive.

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

### Repeatable DSP block benchmark

`skyapo-bench` loads the same `Engine`/upstream filter graph used by the
renderer, warms it up, and reports JSON mean/median/p95/max processing time for
a fixed block, plus that block's nominal time budget. Input copying and config
construction are outside the measured interval. It is a local DSP benchmark,
not a PipeWire callback, scheduling, or end-to-end latency benchmark. Schema
`skyapo-bench-v3` reports process peak RSS and a separate, deliberately narrow
known-buffer payload breakdown: the two upstream `FilterConfiguration` float
sample planes, upstream `DelayFilter` float rings, actual current PDC vector
capacities, and SkyAPO-owned plugin dry-bypass rings. The Delay, bypass and PDC
rings share a 16 MiB per-graph budget; FilterConfiguration scratch has a
separate 64 MiB cap and a 256-lane/65,536-frame dimension limit. The payload
total excludes plugin-private and convolution-internal buffers, PipeWire
runtime scratch, object/pointer storage, allocator metadata, and shared-library
allocations; it is not total graph memory. Ring storage is replaced on
control-thread latency changes so a shrink releases historical capacity. The
16 MiB ring limit is a steady-state schedule limit, not a strict peak-RSS
limit: transactional PDC refresh temporarily keeps the old schedule while
allocating the replacement, so ring payload can approach 32 MiB per Engine.
The scratch, ring and estimated convolution limits are independent; their
nominal known-buffer maximum is 144 MiB per Engine, before PipeWire/runtime
scratch, plugin-private allocations, metadata and allocator overhead. Config
reload may also overlap the active Engine and one candidate. If plugins have
persistent state, the runtime validates old sidecar state, saves the active
state, releases that first candidate, then builds a state-updated candidate;
there is at most one replacement candidate from this state-save path at a
time. An Engine retired after a graph transition may remain alive until all
callbacks that could reference it have drained, so this is not a global
two-Engine guarantee. Nor is it a hard process-memory cap: third-party plugins
and shared libraries can allocate outside SkyAPO's budget.

On 2026-10-14, the new
`skyapo-pipewire-e2e-lv2-pdc-recovery` test exercised an active LV2 latency
increase from 32 to 9999 samples with a 430-term `Copy` fan-in, forcing the
aggregate 16 MiB PDC budget check to reject the new schedule. The test verified
that the daemon PID/control socket and `SkyAPO Virtual Mic` remained present,
the independent PipeWire consumer measured silence, and `skyapo config reload`
returned a failure for an unsupported directive without resuming the invalid
graph. Replacing it with `Preamp: -6 dB` then hard-swapped a new Engine; an
independent consumer measured the expected processed amplitude. While faulted,
the test also forced a PipeWire quantum change from 1024 to 512 frames. The
daemon stayed alive and silent while an invalid config deferred format rebuild;
a valid config reload then rebuilt at the negotiated format and restored
processing. This proves bounded recovery for the deterministic fixture/budget
and invalid-config paths, not rollback of plugin-internal state, recovery from
arbitrary plugin faults, or general third-party plugin stability. The current
Release suite passed 69/69 in 170.70 seconds (34 private PipeWire E2Es), and
the ASan/UBSan suite passed 70/70 in 196.68 seconds (35 private PipeWire
E2Es). These runs also include private PipeWire failure-path tests for CLAP
`CLAP_PROCESS_ERROR` and VST3 `kResultFalse`; they verify fail-closed silence
and daemon survival, not isolation from native plugin crashes or hangs.

Separately, Engine preflight estimates each convolution IR's decoded samples
and the arrays allocated by the pinned `libHybridConv::hcInitSingle()` path,
then enforces a 64 MiB aggregate per-graph convolution-memory budget before
reading the IR payload into memory. The estimate adds 25% workspace headroom
and a fixed allowance for FFTW plans/allocator overhead; it is not included in
the benchmark's known-buffer byte breakdown and is not an exact RSS bound.
`skyapo-core` creates a sparse 60-second stereo float WAV and verifies that
loading it is rejected by this budget before payload decoding. It also checks
the upstream convolution output against direct stereo FIR references for
3-, 257- and 4097-tap impulse responses across fixed blocks.

Example filter-count scaling on a development shell:

```sh
for count in 1 4 16 64; do
  config=$(mktemp)
  for ((i = 0; i < count; i++)); do printf 'Preamp: -0.01 dB\n' >> "$config"; done
  build/skyapo-bench --config "$config" --rate 48000 --channels 2 --block 256
  rm -f "$config"
done
```

To measure a fixed-block convolution graph, point it at a valid convolution
configuration and pass the negotiated rate and block size. Compare runs on the
same host/build; do not interpret `p95_budget_percent` as a guarantee against
PipeWire scheduling jitter.

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

## Remaining limitations / next task

The physical-to-virtual hardware chain, same-rate Convolution, transactional reload, private-graph recovery (source/link loss and PipeWire server restart), mono/stereo routing, 44.1/48/96 kHz negotiation, and LV2/CLAP/VST3 fixture graphs have numerical consumer evidence. Channel and Copy pass offline expected-sample and callback-allocation tests, including intermediate EAPO channels mixed back into the fixed PipeWire virtual-source layout. Full upstream FilterEngine/config-parser compatibility remains incomplete. Convolution hardware rate/quantum variation and physical-device unplug/replug with ordinary desktop clients remain outstanding. LV2/CLAP/VST3 support is limited; CLAP/VST3 standard bypass parameters and LV2's `lv2:enabled` control are synchronized when advertised, while plugins without those controls and FST/VST2 still use host dry bypass. Delay compensation is fixture-backed but end-to-end latency and plugin process isolation remain incomplete; CLAP/LV2/VST3 state persistence has limited fixture-backed tests. FST-based VST2-compatible processing is opt-in and fixture-tested but not production support or validated against third-party VST2 plugins. The Qt editor supports discovered LV2/CLAP/VST3 insertion, writable config-backed parameter editing, bypass controls and Save & Reload; embedded native plugin GUIs, live parameter readback/automation and some workflows remain missing.
