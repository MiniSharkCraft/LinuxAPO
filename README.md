# SkyAPO

SkyAPO 0.9.0 is a daily-use preview of Equalizer APO's Linux audio path—not a 1.0.0 or production-final release. It compiles and invokes selected real upstream Equalizer APO DSP filters; the project does not substitute another audio effects engine. Native PipeWire routes a selected capture device through DSP to `SkyAPO Virtual Mic`. Hardware unplug/replug, full legacy config parity, total end-to-end latency and plugin crash isolation remain incomplete; see “Deferred to 1.0.0” in `ROADMAP.md`. The upstream GPL license and history are kept in `upstream/equalizerapo`; `docs/THIRD_PARTY_NOTICES.md` inventories pinned plugin interfaces and package license files.

## Build

Dependencies: CMake, C++17 compiler, pkg-config, libsndfile, PipeWire development headers/library, and a running user PipeWire/WirePlumber session. The repository carries the SHA-256-pinned BSD-2-Clause MuParserX 3.0.1 source archive from the official Equalizer APO developer wiki; CMake verifies and uses it by default, making the normal build offline-capable. A different copy can be selected with `-DSKYAPO_MUPARSERX_ARCHIVE=/path/to/muparserx_v3_0_1.zip`; setting it empty falls back to the upstream SourceForge URL. MuParserX provides EAPO conditional expressions and semicolon sequencing. FFTW3f is optional; when available it enables the core `GraphicEQ:`/Convolution processing and the editor's upstream GraphicEQ visual controls. Without FFTW3f, the editor preserves `GraphicEQ:` directives as raw text and does not present them as a working visual editor. Pinned CLAP and Steinberg VST3 API/hosting sources are in `upstream/clap` and `upstream/vst3`; Lilv optionally enables LV2 hosting, and Qt 6 Widgets optionally builds the editor. VST3 supports basic single-bus mono/stereo processing and config-time normalized parameter overrides by numeric ID. On Arch Linux: `sudo pacman -S cmake gcc pkgconf libsndfile pipewire wireplumber lilv qt6-base`; install `fftw` as an optional package to enable FFTW3f features.

Upstream is a pinned official SourceForge Git submodule. After cloning SkyAPO, run `git submodule update --init --recursive`. Its checkout remains unmodified: CMake generates a small build-directory compatibility copy of the compiled filters. See `docs/PORTING.md` in the source checkout. SkyAPO uses GPL-licensed Equalizer APO code; upstream copyright notices and `upstream/equalizerapo/License.txt` are preserved.

```sh
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

When Qt 6 Widgets is installed, launch the upstream-based configuration editor with `build/skyapo-ui [config-file]`. It provides daemon/device controls and visual upstream Preamp, parametric-filter and (with FFTW3f) GraphicEQ rows while preserving untouched config lines. GraphicEQ band/table edits update the upstream plot model and are written back to the `GraphicEQ:` line when saved. UI dependency behavior and remaining gaps are documented in `docs/UI.md` in the source checkout.

For AddressSanitizer and UBSan: configure with `-DSKYAPO_SANITIZERS=ON`.

## Offline renderer

```sh
build/skyapo-render --input input.wav --output output.wav --config examples/basic.txt [--device pipewire-node-name]
```

Measure DSP block time for a config (JSON output; this is not PipeWire or
end-to-end latency):

```sh
build/skyapo-bench --config examples/basic.txt --rate 48000 --channels 2 --block 256
```

The WAV sample rate/channel count are preserved. The supported config subset includes `Preamp:`, Equalizer APO parametric and IIR `Filter:`, `Delay:`, `Channel:`, `Copy:`, `Device:`, `Stage:`, `GraphicEQ:`, `Convolution:`, nested `Include:` with relative paths, and basic numeric/boolean `If:`, `ElseIf:`, `Else:`, `EndIf:` directives. `LoudnessCorrection:` is available only in daemon mode when its PipeWire endpoint-volume provider is available. Builds with the opt-in FST VST2 host also accept a restricted `VSTPlugin:` form using a Linux-loadable module, relative paths and named normalized parameters; Windows DLLs and legacy inline `ChunkData` are not supported. The experimental host persists current-program chunks only for modules advertising `ProgramChunks`; fixture tests verify numerical restore and corrupt-state rollback, but do not establish broad third-party compatibility. Conditional variables are `sampleRate`, `inputChannelCount`, and `outputChannelCount`. Invalid and unsupported active lines report the config path and line number. `Stage: capture` selects Linux capture processing; Windows-only `pre-mix`/`post-mix` sections are skipped. This uses upstream filter implementations and factories, but the complete Equalizer APO parser is not yet ported.

Check a config with `build/skyapo config check examples/basic.txt`. `build/skyapo device list` enumerates PipeWire source nodes and reports channel count plus sample rate when node metadata provides it; unavailable rates are shown as `unknown` rather than inferred.

Moving an existing Windows EAPO installation? Follow the migration guide in `docs/MIGRATION.md` in the source checkout and verify the supported subset before using the config with the daemon.

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

## Release packages

Release artifacts are built for x86_64. The Arch `packaging/PKGBUILD` is the native Arch path. For Debian/Ubuntu `.deb` and RPM packages, configure a dedicated Release build with `-DCMAKE_INSTALL_PREFIX=/usr` before running CPack; this keeps the packaged user-service `ExecStart` aligned with the installed binary path. For example, use `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr`, then `cpack --config build-release/CPackConfig.cmake -G DEB` or `-G RPM`. The generic `packaging/create-generic-tarball.sh` archive extracts to a versioned directory and is suitable for rootless private use; follow its included `INSTALL.txt` and do not enable the `/usr`-configured service from a private extraction. Nix users can run `nix build --extra-experimental-features 'nix-command flakes' .#skyapo` or `nix run --extra-experimental-features 'nix-command flakes' .#skyapo -- --version`; `nix flake check --extra-experimental-features 'nix-command flakes'` builds and checks the pinned package. The Nix package intentionally disables the optional Qt UI and hardware-dependent E2E tests.

The previous artifact set and its `SHA256SUMS` remain untouched in `release-artifacts/`; verify with `(cd release-artifacts && sha256sum --check SHA256SUMS)`. Current-source 0.9.0 candidate packages and a separate checksum file are preserved under `release-artifacts/candidate-20261001-v0.9.0-r5/` (Arch pkgrel9, Debian amd64, RPM x86_64 and generic x86_64 tarball). Package smoke checks do not equal dependency-populated clean installs on Debian/Ubuntu or an RPM distribution. The existing annotated `v0.9.0` tag peels to earlier commit `bbc4a158`, not the current candidate source; do not move it or treat it as identifying the candidate. See `ROADMAP.md` for the release-identity gate.

To remove the package without leaving a running daemon, first stop/disable its user service, then remove the package: `systemctl --user disable --now skyapod` and `sudo pacman -Rns skyapo`. This removes application files but intentionally preserves your per-user configuration and state under `$XDG_CONFIG_HOME/skyapo` and `$XDG_STATE_HOME/skyapo`; delete those directories yourself only if you also want to discard settings and saved plugin state.

Select **SkyAPO Virtual Mic** in your recording/application input picker. The physical source is linked using native PipeWire links to planar float DSP ports; actual upstream filters process samples before source output. No external processing processes are spawned. Device selection persists the stable `node.name`, not its runtime numeric ID. Changing selection causes a reconnect. The daemon retries capture-link creation while the selected source and PipeWire connection remain available. If the selected node or PipeWire server disappears, the current runtime may stop; restart it with `skyapo restart` or let the installed user service restart it. SIGINT/SIGTERM cleanly remove its node.

The private PipeWire E2E suite verifies recovery from synthetic source removal/reappearance and server restart. That does not prove physical microphone unplug/replug behavior or desktop clients automatically following a replaced source; those hardware-specific checks remain deferred to 1.0.0.

State lives in `$XDG_CONFIG_HOME/skyapo` (fallback `~/.config/skyapo`); default config is `config.txt`, initialized to unity preamp only when absent. `skyapo start/stop/restart` launches or controls the per-user daemon; start waits for PipeWire format negotiation. `skyapo config show/reload` reads the active config or asks the daemon to validate and swap it. CLI and daemon use a versioned v1 protocol over a mode-0600 Unix socket at `$XDG_RUNTIME_DIR/skyapo.sock`; `skyapo status` reports actual metrics or explicitly reports an unreachable daemon. `skyapo diagnostics --json` emits machine-readable build/runtime status, with unavailable runtime values represented as JSON `null`. The detached CLI-launched daemon logs to the user config directory as `skyapod.log`. Only one daemon can run per runtime directory.

Hardware test (records four seconds of your microphone to the supplied prefix, with explicit physical and virtual targets; requires stereo and the matching SkyAPO config):

```sh
build/skyapo-realtime-probe "$(build/skyapo device current)" build/proof
```

The default is −6 dB at 48 kHz. To test another graph rate, pass expected gain and rate, e.g. `build/skyapo-realtime-probe "$(build/skyapo device current)" build/proof-96k -6 96000`; the PipeWire graph must already be running at that rate. Supported graph rates 44.1, 48 and 96 kHz have been recorded and numerically checked. The probe checks correlation and measured amplitude, not just graph visibility. Recordings contain microphone audio; remove them when no longer needed. Verified realtime results are in `docs/REALTIME.md` in the source checkout.

## Status and limitations

Realtime stereo capture at 44.1/48/96 kHz, virtual-source consumption, -6 dB processing, virtual-node recovery, and callback allocation auditing have been verified on real hardware. DSP supports up to eight recognized speaker positions; graph-rate changes rebuild the DSP off-thread/main-loop while temporarily outputting silence. Other hardware and mono layouts need validation. Quantum is measured from the graph, not guessed.

The adapter uses actual upstream `FilterConfiguration` read/process/write and selected filter implementations, but **not** the complete Windows `FilterEngine` or EAPO parser. Nested `Include:`, capture `Stage:`, `Device:` matching (using Linux PipeWire device identity and upstream AND/OR/GUID rules), transactional hot reload, last-known-good config retention, `Channel:` selection, `Copy:` remapping and numeric/boolean conditionals are implemented; conditions use MuParserX 3.0.1 and support `sampleRate`, `inputChannelCount` and `outputChannelCount`. Full upstream config processing (Windows registry functions and all string/matrix commands) remains unsupported; numeric inline backtick expressions and portable regex helpers are supported. Copy-created intermediate channels are processed in preallocated upstream FilterConfiguration scratch planes and can be mixed back into the fixed PipeWire output; only the physical output layout remains fixed. Upstream GraphicEQ and Convolution/libHybridConv build when FFTW3f is available; convolution uses the negotiated fixed block size. Daemon mode also includes the actual upstream `LoudnessCorrectionFilter` with a conservative live PipeWire volume snapshot; offline renderer/config validation reject this endpoint-dependent command. Native LV2, CLAP and initial single-bus VST3 audio-effect processing are available; plugin details and limitations are in `docs/PLUGINS.md` in the source checkout. Plugin modules run in-process: use only trusted plugins because a plugin crash or hang can take down or stall the daemon. An experimental FST-based legacy VST2-ABI processing path is integrated behind `-DSKYAPO_ENABLE_FST_VST2_HOST=ON`, but disabled in default builds and packages; it is not yet production support or general third-party compatibility. See `docs/VST2_PROTOTYPE.md` for scope, config syntax, tests and the legal/release gate. LV2, CLAP and VST3 support temporary live parameter updates via `skyapo plugin set`; the opt-in VST2 ABI host also supports live updates by numeric index or unique parameter name. Plugin hosts support temporary host-level dry bypass via `skyapo plugin bypass`. All four formats accept config-time parameter overrides; VST2 additionally applies live updates at process-block boundaries. CLAP/LV2/VST3 state persistence has limited fixture-backed support; experimental opt-in VST2 saves/restores current-program chunks only for modules advertising `ProgramChunks`. Plugin UI and crash isolation remain unsupported. Unsupported active-stage commands fail explicitly. Source software volume is unity; use `Preamp:` for gain. Known latency beyond the graph quantum is not estimated.

No heap allocation, parsing, files, enumeration, or logging occurs in the processing callback. Allocation counters cover executable C++ and linked/wrapped C calls, not shared PipeWire library internals. Status timing instrumentation is diagnostic, not a scheduling guarantee.

`skyapo status` and `skyapo diagnostics --json` report the sum of reported plugin latency snapshots when the active hosts provide them (`plugin_reported_latency_sum_samples` and its sample-rate-derived millisecond value). LV2 accepts both its deprecated `reportsLatency` port property and modern `latency` designation; CLAP/VST3 notifications and LV2 polling refresh snapshots off the audio thread. SkyAPO compensates plugin-reported latency and fixed upstream EAPO `Delay:` latency across supported paths, `Copy` fan-ins and final output channels. The sum shown in status is still plugin-reported latency only, not graph-wide or total/end-to-end latency; the graph quantum is a separate scheduling interval. EAPO Delay and PDC rings share one 16 MiB per-graph budget; oversized candidate configs are rejected with file/line diagnostics.

Troubleshooting: run as your normal user, ensure PipeWire/WirePlumber is running, select a physical audio source first, and check daemon stderr/status. A silent mic, wrong source, or failed target makes the recording test fail. Config edits are watched and debounced; invalid replacements leave the last valid graph active, and the error appears in status. Sanitizer builds are for diagnostics, not low-latency production.
