# SkyAPO

SkyAPO is an early Linux port effort for Equalizer APO. It compiles and invokes selected real upstream Equalizer APO DSP filters; the project does not substitute another audio effects engine. The upstream GPL license and history are kept in `upstream/equalizerapo`.

## Build

Dependencies: CMake, C++17 compiler, pkg-config, libsndfile, PipeWire development headers/library, and a running user PipeWire/WirePlumber session. On Arch Linux: `sudo pacman -S cmake gcc pkgconf libsndfile pipewire wireplumber`.

Upstream is a pinned official SourceForge Git submodule. After cloning SkyAPO, run `git submodule update --init --recursive`. Its checkout remains unmodified: CMake generates a small build-directory compatibility copy of the compiled filters. See [porting notes](docs/PORTING.md). SkyAPO uses GPL-licensed Equalizer APO code; upstream copyright notices and `upstream/equalizerapo/License.txt` are preserved.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For AddressSanitizer and UBSan: configure with `-DSKYAPO_SANITIZERS=ON`.

## Offline renderer

```sh
build/skyapo-render --input input.wav --output output.wav --config examples/basic.txt
```

The WAV sample rate/channel count are preserved. Supported commands currently are `Preamp:`, Equalizer APO parametric and IIR `Filter:` commands, and `Delay:`. Invalid and unsupported lines report the config path and line number. This uses upstream filter implementations and factories, but the complete Equalizer APO parser is not yet ported.

Check a config with `build/skyapo config check examples/basic.txt`. `build/skyapo device list` enumerates PipeWire source nodes if PipeWire development files were present at configure time.

## Realtime microphone

```sh
build/skyapo device list
build/skyapo device set <id-or-node-name>
build/skyapo device current
build/skyapod --config examples/preamp.txt
# In another terminal:
build/skyapo status
wpctl status
```

Select **SkyAPO Virtual Mic** in your recording/application input picker. The physical source is linked using native PipeWire links to planar float DSP ports; actual upstream filters process samples before source output. No external processing processes are spawned. Device selection persists the stable `node.name`, not its runtime numeric ID. Changing selection causes a reconnect. The daemon retries transient disconnects and SIGINT/SIGTERM cleanly remove its node.

State lives in `$XDG_CONFIG_HOME/skyapo` (fallback `~/.config/skyapo`); default config is `config.txt`, initialized to unity preamp only when absent. Status uses a private `$XDG_RUNTIME_DIR/skyapo.sock` and reports actual metrics, or explicitly reports an unreachable daemon. Stop with Ctrl-C. Only one daemon can run per runtime directory.

Hardware test (records four seconds of your microphone to the supplied prefix, with explicit physical and virtual targets; requires stereo 48 kHz and `Preamp: -6 dB`):

```sh
build/skyapo-realtime-probe "$(build/skyapo device current)" build/proof
```

This checks correlation and measured amplitude, not just graph visibility. Recordings contain microphone audio; remove them when no longer needed. See [verified realtime results](docs/REALTIME.md).

## Status and limitations

Realtime stereo 48 kHz capture, virtual-source consumption, -6 dB processing, reconnect, and callback allocation auditing have been verified on real hardware. DSP supports up to eight recognized speaker positions; graph-rate changes rebuild the DSP off-thread/main-loop while temporarily outputting silence. Other hardware/rates/layouts need validation. Quantum is measured from the graph, not guessed.

The adapter uses actual upstream filter implementations, **not yet the complete upstream FilterEngine/FilterConfiguration or parser**. Channel/Copy/Include, GraphicEQ, convolution, loudness correction, hot config reload, plugin hosting, service installation, and CLI start/stop/restart are not implemented. Unsupported config commands fail explicitly. Source software volume is fixed at unity; use `Preamp:` for gain. Known latency beyond the graph quantum is not estimated.

No heap allocation, parsing, files, enumeration, or logging occurs in the processing callback. Allocation counters cover executable C++ and linked/wrapped C calls, not shared PipeWire library internals. Status timing instrumentation is diagnostic, not a scheduling guarantee.

Troubleshooting: run as your normal user, ensure PipeWire/WirePlumber is running, select a physical audio source first, and check daemon stderr/status. A silent mic, wrong source, or failed target makes the recording test fail. Config is loaded at startup/reconnect; restart after edits. Sanitizer builds are for diagnostics, not low-latency production.
