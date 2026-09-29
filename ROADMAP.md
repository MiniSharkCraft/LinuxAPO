# SkyAPO roadmap

Current development version: **0.1.0**. The current commit is the first root-repository baseline and the realtime proof in `docs/REALTIME.md` is a regression gate. Roadmap versions are targets, not claims; only tested acceptance criteria advance a milestone. Plan reviewed 2026-09-30.

## 0.1.x — realtime prototype (current baseline)

- [x] Official Equalizer APO SourceForge submodule pinned and clean.
- [x] Actual upstream Preamp, BiQuad, IIR, Delay implementations in Linux build.
- [x] Offline renderer and tests.
- [x] PipeWire device selection, physical capture links, DSP, virtual source, CLI status.
- [x] Independent client records `SkyAPO Virtual Mic`; -6 dB numerical regression.
- [x] Reconnect after virtual node loss; callback allocator audit.
- [x] Linux portability adaptation generated at build time.

## 0.2.x — config compatibility and graph routing

- [x] Implement actual upstream `ChannelFilter` and `CopyFilter` behavior with ordered channel routing and fixed physical output mapping.
- [x] Implement relative/nested `Include`, cycle/depth detection, source/line diagnostics, and last-valid graph preservation during parse failure.
- [x] Add inotify config hot reload with debouncing, control-thread graph creation, atomic pointer swap and deferred retirement after callback quiescence.
- [ ] Expand parser to preserve comments/quoting, validate numeric ranges, and return structured diagnostics.
- [x] Add mono/stereo directive tests and offline expected-sample checks for routing.
- [x] Map `Stage: capture` to the Linux processing path and skip Windows pre/post-mix sections with Include-local stage scope.
- Acceptance: Include trees, Channel selection, Copy remapping and error paths have deterministic offline expected-sample tests; realtime allocation test still passes; -6 dB capture probe still passes.
- [x] Core and allocation tests exercise actual upstream Channel/Copy implementations; new Copy output channels are rejected with file/line because the current virtual source layout is fixed.
- Acceptance: hot reload also passes a live valid→valid and valid→invalid daemon check without changing the active audio chain on failure.
- [x] Live acceptance: an independent PipeWire client measured −3 dB after valid reload, then still measured −3 dB after an invalid replacement; status remained streaming and showed the parse error.

## 0.3.x — upstream configuration engine

- [x] Port `FilterConfiguration` channel-map execution to Linux and preserve upstream `read/process/write` processing.
- [ ] Port `IFilterFactory` lifecycle hooks and use the upstream factory orchestration model.
- [ ] Replace Windows-only `FilterEngine` configuration discovery, synchronization and watcher dependencies with narrow Linux adapters.
- [ ] Evaluate portable expression parser dependencies; document any unsupported expression functions.
- Acceptance: SkyAPO uses upstream configuration/filter orchestration for the supported directives; parser compatibility fixtures from upstream have explicit pass/fail outcomes.

## 0.4.x — robust config lifecycle and PipeWire recovery

- [x] Background-of-callback (PipeWire control-loop) parse/build, safe graph swap, and last-known-good hot reload.
- [ ] Test PipeWire daemon restart and physical device unplug/replug; cover mono/stereo and 44.1/48/96 kHz.
- [ ] Test virtual source consumer churn, daemon restart, overrun reporting and format/quantum changes.
- Acceptance: no callback filesystem/config work or recurring allocations; injected invalid reload preserves audio; recovery tests recreate links/source.

## 0.5.x — remaining core DSP

- [x] Port upstream GraphicEQ and convolution paths with upstream FFTW/libHybridConv behavior where practical (FFTW3f optional; convolution has fixed negotiated block size).
- [ ] Port Channel/Copy/Include conditional details and LoudnessCorrection only where Linux equivalents preserve behavior; document endpoint-volume semantics.
- [ ] Benchmark filter scaling, convolution, memory and latency; add deterministic impulse/frequency-response fixtures.
- Acceptance: every claimed directive has golden reference tests and sanitizer coverage; known latency only is reported.

## 0.6.x — plugin foundation and first native format

- [ ] Format-neutral realtime plugin node API, metadata/state/bypass, failure diagnostics and discovery cache.
- [ ] Implement one native Linux format after evaluating licensing, ABI, realtime guarantees and yabridge wrapper visibility.
- [ ] Add plugin fixtures, scan/load/process/state/failure/latency tests.
- Acceptance: plugin scan stays outside callback; a native test plugin processes in offline and realtime graphs without breaking recovery.

## 0.7.x — plugin formats and yabridge

- [ ] Add remaining CLAP, LV2, VST2 and VST3 support as legally distributable host integrations permit.
- [ ] Verify yabridge-produced wrappers using ordinary native format scanning; document user workflow.
- [ ] Plugin parameters, state, bypass, latency and isolation behavior.
- Acceptance: each format is individually built/tested/reported; no SDK license violations or fabricated support.

## 0.8.x — upstream editor evolution

- [ ] Audit original Equalizer APO Editor projects/toolkit and licensing in detail.
- [ ] Port reusable editor/models toward Qt where feasible; connect only through daemon IPC.
- [ ] Device/status/config editing, live validation, implemented filter rows and actual response analysis.
- Acceptance: GUI edits preserve hand-written config content where possible, errors are actionable, and GUI lifetime is independent of daemon audio.

## 0.9.x — distribution and release hardening

- [x] Implement CLI daemon start/stop/restart, config show/reload, and local Unix-socket control.
- [ ] Version the IPC protocol and add structured diagnostics/filter inspection.
- [ ] User systemd unit, Arch PKGBUILD, install layout, desktop/appstream assets where GUI is ready, man pages.
- [ ] CI: configure/build/CTest/format/sanitizers/package checks without physical hardware.
- [ ] Fresh-install test, licensing/dependency audit, migration docs, release candidate checklist.
- Acceptance: reproducible package install/uninstall and user service; CI and sanitizer suite pass; docs match actual runtime.

## 1.0.0 — release

- [ ] Stable daemon, major EAPO directives, hot reload with rollback.
- [ ] PipeWire capture/virtual mic, recovery, mono/stereo at 44.1/48/96 kHz.
- [x] Preamp, Filter/BiQuad/IIR, Delay, Channel, Copy, Include and Convolution validated; GraphicEQ compiles with its actual upstream implementation.
- [ ] VST2/VST3/LV2/CLAP, yabridge compatibility, plugin state/bypass/failure and known-latency reporting.
- [ ] Ported/evolved original editor; full CLI and diagnostics.
- [ ] Packaging, user service, desktop integration, licensing and clean-install docs.
- [ ] Full regression, sanitizer, hardware, GUI, plugin, packaging and fresh-install test report.

Do not tag or report 1.0.0 until every item has current evidence or the scope is explicitly renegotiated. An unsupported platform or plugin format remains a blocker for 1.0 unless documented and resolved in release scope, not quietly relabeled complete.
