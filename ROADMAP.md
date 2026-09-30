# SkyAPO roadmap

Current development version: **0.1.0**. This root repository tracks the Linux port separately from its pinned official Equalizer APO submodule; the realtime proof in `docs/REALTIME.md` is a regression gate. Roadmap versions are targets, not claims; only tested acceptance criteria advance a milestone. Plan reviewed 2026-09-30.

## 0.1.x — realtime prototype (current baseline)

- [x] Official Equalizer APO SourceForge submodule pinned and clean.
- [x] Actual upstream Preamp, BiQuad, IIR, Delay implementations in Linux build.
- [x] Offline renderer and tests.
- [x] PipeWire device selection, physical capture links, DSP, virtual source, CLI status.
- [x] Independent client records `SkyAPO Virtual Mic`; -6 dB numerical regression.
- [x] Reconnect after virtual node loss; callback allocator audit.
- [x] Show enumerated source channel count and any published sample-rate metadata in CLI and GUI device selectors; unknown rates remain explicit.
- [x] Expose available PipeWire hardware identity properties in `skyapo device list`. Keep persisting `node.name`: the tested workstation publishes no stable serial/bus ID, and `object.serial` is only a runtime identifier.
- [x] GUI device selector refreshes devices/status through CLI, selects by stable node name, serializes selection while requests run, and safely cancels child work when the window closes.
- [x] Linux portability adaptation generated at build time.

## 0.2.x — config compatibility and graph routing

- [x] Implement actual upstream `ChannelFilter` and `CopyFilter` behavior with ordered channel routing and fixed physical output mapping.
- [x] Implement relative/nested `Include`, cycle/depth detection, source/line diagnostics, and last-valid graph preservation during parse failure.
- [x] Add inotify config hot reload with debouncing, control-thread graph creation, atomic pointer swap and deferred retirement after callback quiescence.
- [x] Preserve quoted Include/comment semantics in the parser; preserve untouched config comments/quoting/newlines in the GUI editor; validate numeric ranges and return structured JSON diagnostics with root/include locations. Covered by core/UI/config-check JSON fixtures.
- [x] Reject out-of-domain Preamp/BiQuad gains, frequencies, and Q/slope values with file/line diagnostics; tests cover extreme gain, above-Nyquist frequency, and negative Q.
- [x] Preserve `#` inside double-quoted values (including relative Include filenames) while retaining unquoted trailing-comment behavior.
- [x] Add mono/stereo directive tests and offline expected-sample checks for routing.
- [x] Map `Stage: capture` to the Linux processing path and skip Windows pre/post-mix sections with Include-local stage scope.
- Acceptance: Include trees, Channel selection, Copy remapping and error paths have deterministic offline expected-sample tests; realtime allocation test still passes; -6 dB capture probe still passes.
- [x] Core and allocation tests exercise actual upstream Channel/Copy implementations; new Copy output channels are rejected with file/line because the current virtual source layout is fixed.
- Acceptance: hot reload also passes a live valid→valid and valid→invalid daemon check without changing the active audio chain on failure.
- [x] Live acceptance: an independent PipeWire client measured −3 dB after valid reload, then still measured −3 dB after an invalid replacement; status remained streaming and showed the parse error.

## 0.3.x — upstream configuration engine

- [x] Port `FilterConfiguration` channel-map execution to Linux and preserve upstream `read/process/write` processing.
- [x] Port `IFilterFactory` initialization/configuration/file lifecycle hooks and upstream ordered command dispatch for the supported factories.
- [ ] Replace Windows-only `FilterEngine` configuration discovery, synchronization and watcher dependencies with narrow Linux adapters.
- [x] Use upstream-recommended MuParserX 3.0.1 for production numeric/boolean `If:`/`ElseIf:` conditions, `Eval:` assignments, numeric inline backtick expansion with one context across Includes, and portable upstream `regexSearch`/`regexReplace` callbacks; retain classic muParser as an explicit fallback. Windows registry functions, upstream string-operator overrides and full string/matrix expression semantics remain unsupported.
- [x] Run the unmodified upstream `Setup/config/iir_lowpass.txt` through the production parser/factory/DSP path; measured 1 kHz passband ratio 0.995883 and 12 kHz stopband ratio 0.0395352 at 48 kHz.
- Acceptance: SkyAPO uses upstream configuration/filter orchestration for the supported directives; parser compatibility fixtures from upstream have explicit pass/fail outcomes.

## 0.4.x — robust config lifecycle and PipeWire recovery

- [x] Background-of-callback (PipeWire control-loop) parse/build, safe graph swap, and last-known-good hot reload.
- [x] Negotiate F32P at 44.1/48/96 kHz and verify stereo capture-to-virtual-source recordings with the -6 dB numerical regression.
- [x] Destroy the live SkyAPO virtual node with `pw-cli`; verify daemon retry recreates the source, relinks both physical channels, and an independent recorder still measures the configured DSP gain.
- [x] Restart a private PipeWire server while SkyAPO is processing a deterministic stereo source; verify stable-name rediscovery, virtual source recreation, FL/FR links and the expected −6 dB DSP ratio.
- [x] Exercise a one-channel PipeWire source; verify `MONO` mapping, mono virtual mic, 48 kHz negotiation and −6 dB processing.
- [x] Remove and recreate a selected synthetic source under the same stable name; verify daemon retry, device rediscovery and link/DSP recovery.
- [x] Attach, stop, and reattach a native PipeWire consumer; verify stereo and mono virtual-source samples numerically, including after private server restart.
- [x] Destroy one physical-to-SkyAPO PipeWire Link while both nodes and ports remain; recreate the missing link without restarting the daemon, then independently record and verify the -6 dB chain in mono and stereo CTest graphs.
- [x] Parse PipeWire registry port `node.id` strictly with `std::from_chars`; malformed, partial, and overflowing metadata is ignored rather than throwing through the C callback. Unit-tested empty/junk/whitespace/overflow and valid boundary values.
- [x] Change graph quantum and sample rate while the daemon is live; verify DSP reinitialization and consumer output at 48 kHz/512, 44.1 kHz/512 and 96 kHz/2048.
- [x] Exercise `skyapo restart` against the live desktop PipeWire server with an isolated XDG config; the daemon returned to `streaming`, recreated 2/2 physical links at 48 kHz stereo/1024, and the process PID changed.
- [x] Verify an independent deterministic consumer after daemon restart on a private graph: 144384 stereo frames, measured expected-output RMS ratio 1.000006, and clean recovery with 2/2 links.
- [ ] Test physical device unplug/replug and automatic consumer selection on real desktop clients.
- [x] Exercise overrun reporting under sustained DSP load; an intentionally excessive 2048-BiQuad private-graph test reported every late callback while remaining connected (not a usable realtime chain).
- Acceptance: no callback filesystem/config work or recurring allocations; injected invalid reload preserves audio; recovery tests recreate links/source.

## 0.5.x — remaining core DSP

- [x] Port upstream GraphicEQ and convolution paths with upstream FFTW/libHybridConv behavior where practical (FFTW3f optional; convolution has fixed negotiated block size).
- [x] Reuse upstream `LoudnessCorrectionFilter`/factory in daemon mode with a Linux provider for uniform positive default-render channel gain; atomic worker handoff keeps the callback nonblocking, and config/offline tests cover DSP and unavailable-provider errors.
- [x] Add numerical upstream GraphicEQ response coverage: a 1 kHz sine at 48 kHz measures −6 dB for a `GraphicEQ: 1000 -6` node (0.2 dB tolerance).
- [x] Validate upstream convolution with a three-tap FIR against direct sample-domain reference across three consecutive fixed-size blocks, including stereo independence and state across block boundaries.
- [x] Document that the PipeWire per-channel gain candidate is conservative and is not claimed identical to Windows `IAudioEndpointVolume`; offline render/config check reject the endpoint-dependent command without a live provider.
- [x] Monitor the default PipeWire render sink's effective per-channel `Props` volume and mute state, with live CLI status, metadata/Pod tests, and desktop cross-check against `wpctl`; unrelated ALSA device `Props` must not overwrite the valid volume snapshot.
- [x] Distinguish the diagnostic RMS channel level from a uniform positive per-channel gain candidate; mark asymmetric/zero values unavailable and keep mute separate. Do not treat either candidate as Windows endpoint master-volume semantics without further validation.
- [ ] Benchmark filter scaling, convolution, memory and latency; add deterministic impulse/frequency-response fixtures.
- Acceptance: every claimed directive has golden reference tests and sanitizer coverage; known latency only is reported.

## 0.6.x — plugin foundation and first native format

- [x] Add a format-neutral plugin-instance boundary and native LV2 audio/control-port host through Lilv; wire `Plugin: LV2 <URI>` into the upstream filter graph.
- [x] Add LV2 discovery/list CLI and an ABI-compatible test plugin covering output, realtime block processing and allocation audit.
- [x] Verify the test LV2 plugin on the live physical-to-virtual PipeWire chain with independent recording and combined -12.0206 dB measurement.
- [x] Inspect LV2 control-port symbols/defaults/ranges with `skyapo plugin info`; validate config-time parameter overrides and verify them numerically offline.
- [ ] Live parameter changes, state/bypass, caching, isolation, latency compensation and real third-party plugin coverage.
- Acceptance: plugin scan stays outside callback; a native test plugin processes in offline and realtime graphs without breaking recovery.

## 0.7.x — plugin formats and yabridge

- [x] Add first native CLAP audio-effect host against official pinned CLAP headers; add ID discovery via CLAP_PATH/standard paths and numeric offline/realtime-audit fixtures.
- [x] Add CLAP parameter metadata/CLI inspection and static config-time parameter events/overrides with range validation; test numerical offline processing, invalid config rejection, allocation audit and live PipeWire capture.
- [x] Latch CLAP/VST3 process errors atomically, silence current/following blocks without recalling the failed plugin, and expose plugin identifier/source line through Engine and daemon/CLI status; a private PipeWire CLAP failure graph was independently recorded as 141312 silent frames.
- [x] Pin official Steinberg VST3 SDK components and build the Linux module/hosting subset without adding the full SDK/tutorial/UI tree.
- [x] Add VST3 bundle/class discovery, CLI info, and an initial single-bus mono/stereo audio-effect adapter; manually render a real installed LSP Filter Stereo bundle offline and confirm changed samples.
- [x] Honor `VST3_PATH` alongside Steinberg standard module paths and cache discovered module metadata per process.
- [x] Discover and offline-process the local yabridge ATKExpander VST3 wrapper via the native VST3 host (192000 mono frames at 48 kHz; samples changed).
- [x] Add a redistributable SkyAPO-authored VST3 half-gain fixture; CTest verifies −6.0206 dB offline and 1000 variable realtime DSP blocks pass the allocation audit.
- [x] Verify the VST3 fixture in the physical-mic → PipeWire → DSP → virtual-mic graph on Built-in Audio at 48 kHz stereo/1024 quantum; an independent client recorded 192000 frames with correlation 1, ratio 0.250594 (−12.0206 dB), zero callback allocations/deallocations and zero overruns. Evidence/config: `tests/data/vst3_realtime_chain.txt`.
- [x] Verify the installed LSP Filter Stereo third-party VST3 in the realtime PipeWire graph. With its Bypass parameter enabled, an independent 192000-frame recording measured unity gain/correlation 1; with bypass disabled and Input gain normalized to 0.82, it measured RMS ratio 1.21256 (+1.674 dB) and correlation 0.984988. The gain-only probe's strict correlation gate correctly did not classify this frequency-altering result as a pure-gain pass. Daemon reported zero overruns and zero instrumented callback allocations/deallocations; third-party internal allocations are outside that audit.
- [x] Verify a yabridge-produced VST3 wrapper in a private realtime PipeWire graph: ATKExpander mono processed and reached an independent consumer; its `realtime: no` metadata means this is functional-path evidence, not a real-time-safety guarantee.
- [x] Expose VST3 normalized parameter metadata and validated numeric-ID config overrides; verify default/override audio numerically, metadata CLI, bad-ID/range rejection, and callback allocation audit.
- [x] Pin canonical FST and add an isolated, opt-in VST2-compatible host prototype with a test gain module; validate construction callbacks, mono/stereo planar processing, variable blocks, parameter override, fail-closed oversize handling bounded to negotiated output capacity, and ASan/UBSan. Not connected to Engine/config/package and not third-party compatibility evidence.
- [ ] Add VST3 plugin state/bypass, latency, auxiliary buses and plugin failure isolation.
- [!] The Steinberg-SDK VST2 route remains license-blocked. An isolated FST-based compatibility prototype now exists without Steinberg/EAPO VST2 headers, but FST is incomplete and its GPL/trademark implications and third-party compatibility are not resolved; this is not production VST2 support. Keep the 1.0 acceptance open pending licensing review and broader validation. See `docs/PLUGINS.md` and `docs/VST2_PROTOTYPE.md`.
- [ ] Test yabridge VST2 wrappers if a lawful host path becomes available.
- [ ] Add CLAP live parameter/control changes, state/latency support, production-plugin validation and safer failure isolation.
- [ ] Expand yabridge verification to additional/stereo-compatible wrappers, address plugin-reported non-realtime behavior/isolation, and test yabridge VST2 wrappers if a lawful host path becomes available.
- [ ] Plugin live parameters, state, bypass, latency and isolation behavior.
- Acceptance: each format is individually built/tested/reported; no SDK license violations or fabricated support.

## 0.8.x — upstream editor evolution

- [x] Inspect the original Qt Widgets/qmake Editor, reusable filter/analysis widgets and Windows-only device/APO/registry/VST UI integrations.
- [ ] Audit GUI-specific resource/dependency licensing and establish a Qt 6 build for selected upstream components.
- [x] Build the first Qt 6 daemon-client editor using actual upstream Preamp/BiQuad/Delay/Stage widgets and factories, with byte-preserving config editing and widget-serialization tests.
- [ ] Port additional reusable upstream editor/analysis components; Windows-bound full FilterTable factory/device integration remains excluded.
- [x] Run GUI-side CLI control requests asynchronously and coalesce periodic status/device queries.
- [x] Reuse the actual upstream `FilterTableRow` widget/resources through a narrow Linux adapter; full upstream table model/selection/drag behavior remains open.
- [x] Add ordered row selection/focus, Ctrl/Shift range selection, Escape clear, Delete removal and Alt+Up/Down reordering; preserve config bytes and newline style on move.
- [x] Add a Linux Include editor with config-relative path validation and native file browsing; upstream Include's Registry ACL and tab-navigation hooks are intentionally not reused.
- [x] Add Linux visual `Channel:`/`Copy:` editors with EAPO serialization and explicit raw-text fallback for unsupported syntax; upstream widgets remain excluded due to Windows device/channel-mask dependencies.
- [x] Add a visual IIR coefficient editor with ordering/range validation, round-trip tests and raw-expression fallback; the upstream Editor has no IIR visual editor to reuse.
- [ ] Device/status/config editing, live validation, implemented filter rows and actual response analysis.
- Acceptance: GUI edits preserve hand-written config content where possible, errors are actionable, and GUI lifetime is independent of daemon audio.

## 0.9.x — distribution and release hardening

- [x] Implement CLI daemon start/stop/restart, config show/reload, and local Unix-socket control.
- [x] Add actual active filter source-line listing and build/upstream revision diagnostics.
- [x] Include the linked PipeWire library version in `skyapo diagnostics`; it is reported from the runtime library, not hardcoded.
- [x] Report a connected-but-silent daemon control socket as unresponsive; `skyapo status` now exits with an actionable error instead of printing an empty response.
- [x] Version the local IPC protocol with bounded v1 request/response framing and rejection tests.
- [x] Add tested structured/machine-readable CLI diagnostics (`skyapo diagnostics --json`).
- [x] User systemd unit, Arch PKGBUILD, CMake install rules, and upstream license installation.
- [x] Desktop launcher for the Qt editor.
- [x] AppStream metadata, branded icon and man pages. No MIME association is declared: current EAPO configs are ordinary `.txt` files, and claiming that broad MIME type would hijack unrelated text documents.
- [x] Install the original Equalizer APO GPL text and pinned CLAP/VST3 MIT license texts with the package; staged CMake install test verifies each file and the third-party notice inventory.
- [x] Add GitHub Actions Linux configure/build/CTest and ASan/UBSan jobs without physical hardware; actual hosted results remain pending because this local repository has no configured remote.
- [x] Add opt-in isolated PipeWire E2E CTest: launch private server + deterministic capture source + `skyapod` + independent virtual-mic consumer; verify runtime status and -6 dB output. Enabled in the normal hosted Linux CI job; hosted result still pending.
- [x] Add an Arch container CI job that builds the PKGBUILD and runs its package CTest suite.
- [x] Add an incremental `clang-format` CI check for changed SkyAPO-owned C++ files, excluding upstream and generated code; verify hosted workflow runs after publishing/connecting the repository.
- [x] Document Windows Equalizer APO → Linux SkyAPO config migration and compatibility gaps.
- [x] Audit Arch package license placement with `namcap`; install all license texts under `/usr/share/licenses/skyapo/` and verify an isolated package archive.
- [x] Run a clean staged Release build/install smoke; installed CLI/config-check/render, service, desktop metadata, man pages and license notices were inspected. Correct stale LoudnessCorrection docs and avoid linking source-only docs from the installed README.
- [ ] Fresh-install test, licensing/dependency audit, release candidate checklist.
- Acceptance: reproducible package install/uninstall and user service; CI and sanitizer suite pass; docs match actual runtime.

## 1.0.0 — release

- [ ] Stable daemon, major EAPO directives, hot reload with rollback.
- [ ] PipeWire capture/virtual mic, recovery, mono/stereo at 44.1/48/96 kHz.
- [x] Preamp, Filter/BiQuad/IIR, Delay, Channel, Copy, Include, three-tap Convolution and a 1 kHz GraphicEQ point validated with numerical checks against known output.
- [ ] VST2/VST3/LV2/CLAP product-grade support, yabridge compatibility, plugin state/bypass/failure and known-latency reporting. LV2/CLAP and initial single-bus VST3 hosts are prototypes, not full 1.0 acceptance; one mono yabridge wrapper works functionally but reports `realtime: no`, and VST2 remains license-blocked.
- [ ] Ported/evolved original editor; full CLI and diagnostics.
- [ ] Packaging, user service, desktop integration, licensing and clean-install docs.
- [ ] Full regression, sanitizer, hardware, GUI, plugin, packaging and fresh-install test report.

Do not tag or report 1.0.0 until every item has current evidence or the scope is explicitly renegotiated. An unsupported platform or plugin format remains a blocker for 1.0 unless documented and resolved in release scope, not quietly relabeled complete.
