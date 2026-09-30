# SkyAPO roadmap

Current development version: **0.1.0**. The current commit is the first root-repository baseline and the realtime proof in `docs/REALTIME.md` is a regression gate. Roadmap versions are targets, not claims; only tested acceptance criteria advance a milestone. Plan reviewed 2026-09-30.

## 0.1.x — realtime prototype (current baseline)

- [x] Official Equalizer APO SourceForge submodule pinned and clean.
- [x] Actual upstream Preamp, BiQuad, IIR, Delay implementations in Linux build.
- [x] Offline renderer and tests.
- [x] PipeWire device selection, physical capture links, DSP, virtual source, CLI status.
- [x] Independent client records `SkyAPO Virtual Mic`; -6 dB numerical regression.
- [x] Reconnect after virtual node loss; callback allocator audit.
- [x] Show enumerated source channel count and any published sample-rate metadata in CLI and GUI device selectors; unknown rates remain explicit.
- [x] Linux portability adaptation generated at build time.

## 0.2.x — config compatibility and graph routing

- [x] Implement actual upstream `ChannelFilter` and `CopyFilter` behavior with ordered channel routing and fixed physical output mapping.
- [x] Implement relative/nested `Include`, cycle/depth detection, source/line diagnostics, and last-valid graph preservation during parse failure.
- [x] Add inotify config hot reload with debouncing, control-thread graph creation, atomic pointer swap and deferred retirement after callback quiescence.
- [ ] Expand parser to preserve comments/quoting, validate numeric ranges, and return structured diagnostics.
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
- [ ] Evaluate portable expression parser dependencies; document any unsupported expression functions.
- Acceptance: SkyAPO uses upstream configuration/filter orchestration for the supported directives; parser compatibility fixtures from upstream have explicit pass/fail outcomes.

## 0.4.x — robust config lifecycle and PipeWire recovery

- [x] Background-of-callback (PipeWire control-loop) parse/build, safe graph swap, and last-known-good hot reload.
- [x] Negotiate F32P at 44.1/48/96 kHz and verify stereo capture-to-virtual-source recordings with the -6 dB numerical regression.
- [ ] Test PipeWire daemon restart and physical device unplug/replug; verify mono capture and virtual source consumer churn.
- [ ] Test virtual source consumer churn, daemon restart, overrun reporting and format/quantum changes.
- Acceptance: no callback filesystem/config work or recurring allocations; injected invalid reload preserves audio; recovery tests recreate links/source.

## 0.5.x — remaining core DSP

- [x] Port upstream GraphicEQ and convolution paths with upstream FFTW/libHybridConv behavior where practical (FFTW3f optional; convolution has fixed negotiated block size).
- [ ] Port Channel/Copy/Include conditional details and LoudnessCorrection only where Linux equivalents preserve behavior; document endpoint-volume semantics.
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
- [ ] Add CLAP live parameter/control changes, state/latency support, production-plugin validation and safer failure isolation.
- [ ] Add VST2 and VST3 support as legally distributable host integrations permit.
- [ ] Verify yabridge-produced wrappers using ordinary native format scanning; document user workflow.
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
- [ ] Device/status/config editing, live validation, implemented filter rows and actual response analysis.
- Acceptance: GUI edits preserve hand-written config content where possible, errors are actionable, and GUI lifetime is independent of daemon audio.

## 0.9.x — distribution and release hardening

- [x] Implement CLI daemon start/stop/restart, config show/reload, and local Unix-socket control.
- [x] Add actual active filter source-line listing and build/upstream revision diagnostics.
- [ ] Version the IPC protocol and add structured/machine-readable diagnostics.
- [x] User systemd unit, Arch PKGBUILD, CMake install rules, and upstream license installation.
- [x] Desktop launcher for the Qt editor.
- [ ] AppStream metadata, branded icon, MIME association and man pages.
- [ ] CI: configure/build/CTest/format/sanitizers/package checks without physical hardware.
- [ ] Fresh-install test, licensing/dependency audit, migration docs, release candidate checklist.
- Acceptance: reproducible package install/uninstall and user service; CI and sanitizer suite pass; docs match actual runtime.

## 1.0.0 — release

- [ ] Stable daemon, major EAPO directives, hot reload with rollback.
- [ ] PipeWire capture/virtual mic, recovery, mono/stereo at 44.1/48/96 kHz.
- [x] Preamp, Filter/BiQuad/IIR, Delay, Channel, Copy, Include and Convolution validated; GraphicEQ compiles with its actual upstream implementation.
- [ ] VST2/VST3/LV2/CLAP product-grade support, yabridge compatibility, plugin state/bypass/failure and known-latency reporting. Current native LV2 and first CLAP hosts are prototypes, not full 1.0 acceptance.
- [ ] Ported/evolved original editor; full CLI and diagnostics.
- [ ] Packaging, user service, desktop integration, licensing and clean-install docs.
- [ ] Full regression, sanitizer, hardware, GUI, plugin, packaging and fresh-install test report.

Do not tag or report 1.0.0 until every item has current evidence or the scope is explicitly renegotiated. An unsupported platform or plugin format remains a blocker for 1.0 unless documented and resolved in release scope, not quietly relabeled complete.
