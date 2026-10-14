# LinuxAPO release roadmap

This roadmap follows the source-level comparison in [PORT_GAP_AUDIT.md](PORT_GAP_AUDIT.md). LinuxAPO is currently version **0.9.0 beta**; the published 0.9.0 release/tag is immutable. Based on the substantial user-visible capability growth since that baseline, the recommended next release is **0.10.0** after its gates pass. A 0.9.1 remains an optional bug-fix-only release if an urgent supported-path regression needs shipping first. Neither version is approved or released. Versions below are planning targets, not claims that their acceptance criteria have passed.

## Current baseline — 0.9.0 beta

The baseline provides a PipeWire capture → upstream-backed EAPO DSP → virtual microphone path; a daemon, CLI and Qt config editor; offline rendering; a tested subset of EAPO directives; and native LV2, CLAP and single-bus VST3 hosting. FST-based VST2 is opt-in and experimental. It is not full Equalizer APO parity, not production-final and not a guarantee for arbitrary devices or plugins. See the audit and [REALTIME.md](REALTIME.md) for evidence and limits.

## Conditional patch — 0.9.1 (only if urgent fixes need shipping first)

Priority: P0 only if a confirmed release-blocking defect warrants an interim release; otherwise skip this patch. Estimate: **5–10 engineering days**, assuming one Linux/PipeWire development host, existing fixtures and no new public API. This estimate excludes external review and hardware diversity.

Deliverables:

- Fix only confirmed regressions in the supported capture/DSP/virtual-source path, config checking/reload, supported plugin hosts and packaging.
- Resolve the observed editor-versus-daemon config/status discrepancy if reproducible; make the loaded config identity and active processing diagnostics unambiguous.
- Retain bounded memory behavior for filter scratch, convolution, delay, PDC and plugin bypass; fix any incorrect reconfiguration/accounting discovered by tests.
- Correct actionable error reporting for unsupported stages, invalid convolution inputs, unavailable devices and plugin failures. Do not silently claim pre/post-mix playback processing.
- Preserve package install/upgrade/removal behavior and current 0.9.0 compatibility; no version bump until separately approved.

Dependencies: reproduce each alleged defect; supported-format fixtures; existing private PipeWire graph and package CI.

Acceptance and tests:

- Full normal CTest and ASan/UBSan suites pass from a clean configured build; test counts/results are recorded with the exact source revision.
- Offline renderer and config-check tests cover saved, included, invalid and unsupported directives; Preamp and filter numerical references remain within their established tolerances.
- PipeWire independent-consumer E2E verifies actual processed samples, reload rollback, switching/reconnect/restart, mono/stereo and supported sample rates; callback allocation audit remains zero.
- Plugin fixture tests cover supported formats, state/control/bypass/latency paths; clearly mark arbitrary third-party plugin behavior unverified.
- Arch package build/install/remove smoke passes. Other package formats receive focused smoke tests when their builders are available.
- No unresolved P0 crash, hang, unbounded allocation or data-loss regression in supported daily-use paths.

Difficulty: medium/high because lifecycle, graph reconfiguration and plugin timing defects require deterministic reproduction. Release blocker: any failed supported-path regression or unreviewed dirty source.

Explicitly deferred: playback processing, full EAPO language parity, new plugin formats, native plugin GUI, complete response/phase/latency analysis, plugin process isolation, universal hardware certification and legal clearance for experimental VST2 distribution.

## Recommended next release — 0.10.0 (feature and compatibility milestone)

Priority: P0 for the next planned release, after the focused correctness/release blockers below are closed. Estimate: **20–40 engineering days**, dependent on UX decisions and backend boundaries; this is a planning range, not a calendar commitment.

The version step is justified by the post-0.9.0 feature delta already present on the current development branch: expanded device/editor workflows, plugin control/state/PDC, response analysis and broader PipeWire regression coverage. It does not imply full EAPO parity. Deliverables, ordered by functional value:

1. Decide and implement the supported playback/sink-processing graph if it is required; retain capture-to-virtual-mic as a separately reliable route.
2. Improve editor usability for channel routing, GraphicEQ and convolution; add clear diagnostics for active versus unsupported EAPO stages and config commands.
3. Provide usable plugin parameter/state workflows for supported hosts, and expose only capabilities verified by each format. Native plugin GUI embedding is a separate scoped decision.
4. Expand response analysis (documented channel semantics and useful magnitude/phase/latency views) without presenting theoretical analysis as measured hardware latency.
5. Improve endpoint selection/troubleshooting and device identity behavior across more PipeWire implementations.

Dependencies: 0.9.1 regression baseline; user workflows ranked from the parity audit; licensing review before enabling FST/VST2 by default.

Acceptance and tests: each user workflow has UI and backend integration coverage; tests assert the saved config actually changes Engine output; response plots have numeric fixtures; playback, if included, has independent PipeWire producer/consumer tests; no desktop-only feature may bypass the daemon's validation/runtime path.

Difficulty: high. Release blockers: feature scope must be explicit, each feature must be wired end-to-end, and unsupported formats/stages must remain visibly unsupported.

Deferred here unless separately prioritized: Windows COM/Registry/installer behavior, plugin crash sandboxing, broad yabridge certification and cosmetic replication of the upstream editor.

## Stability milestone — 1.0.0

Priority: P0 release gate after the feature scope is frozen. Estimate: **10–20 engineering days after 0.10 scope stabilizes**, plus external hardware, legal and clean-install validation that cannot be estimated as local coding time.

Required deliverables:

- Freeze a documented supported config, device, plugin and package matrix; identify intentional Linux behavior differences.
- No known critical correctness, crash, deadlock, unbounded memory or config-loss issue in supported workflows.
- Transactional config reload and recovery verified under daemon, PipeWire, device and plugin lifecycle faults.
- Supported plugin formats meet published state, latency/PDC and failure-reporting requirements; VST2 remains excluded or experimental unless licensing/distribution and compatibility gates are resolved.
- Reproducible release artifacts, checksums, install/upgrade/uninstall verification and complete license/attribution inventory.
- Clean install and upgrade validation on each declared distribution; real hardware tests across representative input devices and channel layouts.
- User documentation, diagnostics and known limitations match the release binaries.

Acceptance and tests:

- Full CTest, ASan/UBSan, PipeWire E2E, renderer/config, plugin fixture, daemon restart/recovery, allocation, PDC/latency and package smoke suites pass on the tagged source.
- Independent audio recording proves capture → DSP → virtual source on each claimed hardware class. Synthetic PipeWire tests do not substitute for this gate.
- Test at least one clean install, upgrade and uninstall per declared package family; Nix flake check/build on a clean supported evaluator.
- Independent license review for all bundled/linked components and enabled plugin formats.
- Tag only a clean, reviewed commit after all declared gates pass; retain exact evidence/artifact hashes.

Difficulty: high. External blockers include hardware diversity, distribution-specific transaction tests, third-party plugin behavior and qualified licensing review.

## Longer-term optional work

- Plugin subprocess isolation and recovery; automation and richer plugin state UI.
- Additional plugin formats only when they can be supported and tested without compromising realtime safety.
- More complete response, phase and measured-latency diagnostics.
- Additional distributions and installation channels after the 1.0 package matrix is stable.

These items are not prerequisites for 1.0 unless promoted into the explicitly supported product scope. Windows-only APO registration, COM activation, Registry installation and Windows service management are not LinuxAPO goals; PipeWire-native equivalents are evaluated by behavior, not by literal API similarity.
