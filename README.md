# SkyAPO

SkyAPO is an early Linux port effort for Equalizer APO. It compiles and invokes selected real upstream Equalizer APO DSP filters; the project does not substitute another audio effects engine. The upstream GPL license and history are kept in `upstream/equalizerapo`.

## Build

Dependencies: CMake, C++17 compiler, pkg-config, libsndfile, PipeWire development headers/library, and a running user PipeWire/WirePlumber session. FFTW3f is optional; when available it enables the core `GraphicEQ:`/Convolution processing and the editor's upstream GraphicEQ visual controls. Without FFTW3f, the editor preserves `GraphicEQ:` directives as raw text and does not present them as a working visual editor. Pinned CLAP and Steinberg VST3 API/hosting sources are in `upstream/clap` and `upstream/vst3`; Lilv optionally enables LV2 hosting, and Qt 6 Widgets optionally builds the editor. VST3 supports basic single-bus mono/stereo processing and config-time normalized parameter overrides by numeric ID. On Arch Linux: `sudo pacman -S cmake gcc pkgconf libsndfile pipewire wireplumber lilv qt6-base`; install `fftw` as an optional package to enable FFTW3f features.

Upstream is a pinned official SourceForge Git submodule. After cloning SkyAPO, run `git submodule update --init --recursive`. Its checkout remains unmodified: CMake generates a small build-directory compatibility copy of the compiled filters. See [porting notes](docs/PORTING.md). SkyAPO uses GPL-licensed Equalizer APO code; upstream copyright notices and `upstream/equalizerapo/License.txt` are preserved.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

When Qt 6 Widgets is installed, launch the upstream-based configuration editor with `build/skyapo-ui [config-file]`. It provides daemon/device controls and visual upstream Preamp, parametric-filter and (with FFTW3f) GraphicEQ rows while preserving untouched config lines. GraphicEQ band/table edits update the upstream plot model and are written back to the `GraphicEQ:` line when saved. See [UI status](docs/UI.md) for dependency behavior and remaining gaps.

For AddressSanitizer and UBSan: configure with `-DSKYAPO_SANITIZERS=ON`.

## Offline renderer

```sh
build/skyapo-render --input input.wav --output output.wav --config examples/basic.txt
```

The WAV sample rate/channel count are preserved. Supported commands currently are `Preamp:`, Equalizer APO parametric and IIR `Filter:`, `Delay:`, `Channel:`, `Copy:`, `Stage:`, `GraphicEQ:`, `Convolution:`, and nested `Include:` with relative paths. Invalid and unsupported active lines report the config path and line number. `Stage: capture` selects Linux capture processing; Windows-only `pre-mix`/`post-mix` sections are skipped. This uses upstream filter implementations and factories, but the complete Equalizer APO parser is not yet ported.

Check a config with `build/skyapo config check examples/basic.txt`. `build/skyapo device list` enumerates PipeWire source nodes and reports channel count plus sample rate when node metadata provides it; unavailable rates are shown as `unknown` rather than inferred.

## Realtime microphone

```sh
build/skyapo device list
build/skyapo device set <id-or-node-name>
build/skyapo device current
build/skyapod --config examples/preamp.txt
# In another terminal:
build/skyapo status
build/skyapo diagnostics --json
build/skyapo config reload
build/skyapo filters
build/skyapo diagnostics
build/skyapo stop
wpctl status
```

After installing the package, enable the user service with `systemctl --user enable --now skyapod`. On Arch Linux, build/install the local PKGBUILD from the repository with `cd packaging && makepkg -si`; select the input device first. The service is user-scoped and does not require a root daemon.

Select **SkyAPO Virtual Mic** in your recording/application input picker. The physical source is linked using native PipeWire links to planar float DSP ports; actual upstream filters process samples before source output. No external processing processes are spawned. Device selection persists the stable `node.name`, not its runtime numeric ID. Changing selection causes a reconnect. The daemon retries transient disconnects and SIGINT/SIGTERM cleanly remove its node.

State lives in `$XDG_CONFIG_HOME/skyapo` (fallback `~/.config/skyapo`); default config is `config.txt`, initialized to unity preamp only when absent. `skyapo start/stop/restart` launches or controls the per-user daemon; start waits for PipeWire format negotiation. `skyapo config show/reload` reads the active config or asks the daemon to validate and swap it. CLI and daemon use a versioned v1 protocol over a mode-0600 Unix socket at `$XDG_RUNTIME_DIR/skyapo.sock`; `skyapo status` reports actual metrics or explicitly reports an unreachable daemon. `skyapo diagnostics --json` emits machine-readable build/runtime status, with unavailable runtime values represented as JSON `null`. The detached CLI-launched daemon logs to the user config directory as `skyapod.log`. Only one daemon can run per runtime directory.

Hardware test (records four seconds of your microphone to the supplied prefix, with explicit physical and virtual targets; requires stereo and the matching SkyAPO config):

```sh
build/skyapo-realtime-probe "$(build/skyapo device current)" build/proof
```

The default is −6 dB at 48 kHz. To test another graph rate, pass expected gain and rate, e.g. `build/skyapo-realtime-probe "$(build/skyapo device current)" build/proof-96k -6 96000`; the PipeWire graph must already be running at that rate. Supported graph rates 44.1, 48 and 96 kHz have been recorded and numerically checked. The probe checks correlation and measured amplitude, not just graph visibility. Recordings contain microphone audio; remove them when no longer needed. See [verified realtime results](docs/REALTIME.md).

## Status and limitations

Realtime stereo capture at 44.1/48/96 kHz, virtual-source consumption, -6 dB processing, virtual-node recovery, and callback allocation auditing have been verified on real hardware. DSP supports up to eight recognized speaker positions; graph-rate changes rebuild the DSP off-thread/main-loop while temporarily outputting silence. Other hardware and mono layouts need validation. Quantum is measured from the graph, not guessed.

The adapter uses actual upstream `FilterConfiguration` read/process/write and selected filter implementations, but **not** the complete Windows `FilterEngine` or EAPO parser. Nested `Include:`, capture `Stage:`, transactional hot reload, `Channel:` selection and `Copy:` remapping are implemented. The fixed PipeWire source cannot expose extra channels created by Copy, so those configs fail with a source/line error. Upstream GraphicEQ and Convolution/libHybridConv build when FFTW3f is available; convolution uses the negotiated fixed block size. Native LV2, CLAP and initial single-bus VST3 audio-effect processing are available; see [plugin support and limitations](docs/PLUGINS.md). Loudness correction, VST2, live plugin parameter changes/state/bypass/UI and advanced conditional parsing remain unsupported. VST3 and CLAP accept config-time parameter overrides; live parameter changes remain unsupported. Unsupported active-stage commands fail explicitly. Source software volume is unity; use `Preamp:` for gain. Known latency beyond the graph quantum is not estimated.

No heap allocation, parsing, files, enumeration, or logging occurs in the processing callback. Allocation counters cover executable C++ and linked/wrapped C calls, not shared PipeWire library internals. Status timing instrumentation is diagnostic, not a scheduling guarantee.

Troubleshooting: run as your normal user, ensure PipeWire/WirePlumber is running, select a physical audio source first, and check daemon stderr/status. A silent mic, wrong source, or failed target makes the recording test fail. Config edits are watched and debounced; invalid replacements leave the last valid graph active, and the error appears in status. Sanitizer builds are for diagnostics, not low-latency production.
