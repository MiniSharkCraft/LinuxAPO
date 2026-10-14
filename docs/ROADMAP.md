# LinuxAPO incremental release roadmap

LinuxAPO's published baseline is **v0.9.0 Beta**. Its tag and release are
immutable. This file is the canonical release sequence; the root
[`ROADMAP.md`](../ROADMAP.md) retains the detailed historical acceptance and
test-evidence log. Milestones below are planned gates, not claims of completion
or release dates. Releases in the 0.x series are GitHub prereleases.

## Release metadata and automation

For each release, prepare a distinct `docs/releases/vX.Y.Z.md` from the
verified changes and test evidence; update `CHANGELOG.md`, the README's
released-version links, `CMakeLists.txt`, and affected user/developer docs in
the same final release-preparation commit. Annotated tags use that release
file's exact first-level title followed by a concise tag summary. Do not create
the notes from a tag workflow or tag an unfinished worktree.

`.github/workflows/release.yml` is the single tag-triggered publisher. It
requires an annotated `v0.x.y` tag matching CMake, a matching changelog entry,
committed version-specific notes, and verified build/test/package jobs. It
creates a draft prerelease only after all jobs pass, downloads the uploaded
assets back, verifies `SHA256SUMS`, and only then makes the prerelease public.
The workflow rejects an existing release and does not run for v1.0.0. The
publish job alone has `contents: write`; development commits continue through
ordinary CI without producing releases.

## Baseline and current checkpoint

The 0.9.0 baseline provides an upstream-backed EAPO DSP path from PipeWire
capture to the `SkyAPO Virtual Mic`, offline rendering, CLI/daemon and a Qt
editor. LinuxAPO also contains native LV2, CLAP and single-audio-bus VST3
hosts; FST-based VST2 compatibility is optional and experimental. This is not
full EAPO parity, system-wide playback processing, or production-final.

The active unpublished checkpoint is **post-0.9.0 runtime hardening**. The
worktree currently contains uncommitted runtime, DSP, UI, packaging, test and
documentation changes; none is treated as released. New focused PipeWire E2E
tests for LV2 and VST3 state persistence across a fresh daemon restart passed
1/1 each in Release and ASan/UBSan; subsequent full suites passed **76/76**
Release CTest and **77/77** ASan/UBSan CTest (42 private PipeWire E2Es in the
latter). Current-source DEB, RPM and generic `.tar.zst` extracted-payload
smokes and `-6 dB` renderer checks also passed. These are local dirty-worktree
results, not hosted CI on a clean commit or hardware diversity validation; CI
must retest the exact checkpoint commit before a release.

### Next candidate — v0.9.1 Beta: supported-path correctness and release hygiene

**Purpose:** ship only the accumulated compatible hardening that can pass the
release gates, without implying playback or broad EAPO/plugin parity.

**Scope:** bounded memory and transactional PDC/reconfiguration; actionable
fail-closed behavior for runtime/plugin errors; DSP/config boundary checks;
repeatable callback, latency and state-restart tests; disk-backed temporary
staging; package/install smoke corrections. Optional experimental hosts stay
disabled in default packages. The unverified application artwork currently in
the worktree is excluded from public commit/package/release until its
redistribution rights are confirmed; the rest of the candidate must not depend
on shipping it.

**Acceptance criteria:**

- Full Release CTest and ASan/UBSan suites pass on the exact candidate commit;
  no current supported-path P0/P1 defect remains.
- PipeWire independent-consumer E2Es verify processed audio, config rollback,
  source/server recovery, supported channel/rate transitions, and plugin
  state/reload paths. Runtime callback allocation/deallocation counters remain
  zero in steady state.
- Renderer/config numerical tests verify Preamp, filter, convolution, delay,
  channel routing and invalid-config rollback wherever implemented.
- Package smoke covers every artifact being published, with install paths,
  desktop metadata, service files and notices verified. Unsupported package
  families are not advertised as validated.
- `git status` is clean at the release commit; source, submodules and licenses
  are audited; no unlicensed artwork, generated binaries, cache or secrets are
  included.

**Required tests:** full CTest; ASan/UBSan; PipeWire producer→daemon→independent
consumer; renderer/config; plugin fixtures and state restart; callback
allocation invariants; PDC/latency tests; package install/remove smoke; Nix
checks if its derivation changes. Hosted CI must pass on the release commit.

**Hardware/external validation:** physical capture-to-virtual-mic smoke on the
available desktop is required if the device/server is accessible. Broader
device diversity and independent third-party plugin compatibility remain
unproven and are not advertised as covered by fixture tests.

**Known limitations:** playback is not processed system-wide; arbitrary
third-party plugin crashes/hangs are not isolated; hardware diversity, total
ADC-to-application latency and several distribution install matrices are not
proven. FST/VST2 remains experimental and subject to legal review.

**Release readiness:** publish/tag v0.9.1 only after all applicable criteria
pass on one reviewed, clean commit. If the icon-rights dependency cannot be
removed from the candidate or clarified, publishing is blocked; preserve the
assets locally without redistributing them.

### v0.10.0 Beta: PipeWire playback processing

**Scope:** add a native PipeWire playback graph for selected application audio,
process it with the same EAPO-backed engine, and deliver it to a selected
physical sink. Keep capture-to-virtual-microphone as an independent supported
route. Specify graph ownership, default/fallback behavior and loop/feedback
prevention before implementation.

**Acceptance criteria:**

- A native PipeWire test producer's known signal passes through LinuxAPO DSP
  and is captured by an independent consumer attached to the intended physical
  sink/monitor path; numeric gain/filter output matches reference tolerances.
- UI/CLI/status report selected sink, graph state, negotiated format and
  unavailable/unsupported playback states without fabricated values.
- Capture mode remains regression-clean; selecting playback cannot create a
  capture/playback feedback loop or silently redirect unrelated streams.
- Graph teardown, daemon restart, PipeWire restart, device removal and config
  reload fail safely and recover when the endpoint returns.
- No recurring realtime heap allocation; bounded buffers and measured graph
  latency are reported honestly.

**Required tests:** private PipeWire producer→LinuxAPO→sink/monitor→independent
consumer E2E; unity and non-unity DSP numeric checks; routing/feedback,
format/quantum changes, daemon and server restart, capture regression,
allocation, sanitizer and latency tests.

**Hardware/external validation:** verify audible playback on at least one real
physical output and capture a loopback/monitor recording. Verify no feedback
under ordinary desktop playback. Other audio servers and device diversity are
not implied.

**Known limitations:** initial support may be limited to one selected sink and
PipeWire-managed application streams; per-application routing, simultaneous
multi-sink graphs and JACK/PulseAudio-specific behavior are out of scope unless
the tested design requires them.

**Release readiness:** publish only after real playback and independent
recording evidence exists and capture/virtual-mic acceptance remains green.

### v0.11.0 Beta: endpoint selection and lifecycle reliability

**Scope:** close user-facing gaps in input/output device identity, switching,
hotplug, default-device changes and PipeWire reconnect/recovery across capture
and playback routes.

**Acceptance criteria:** stable identifiers survive runtime numeric-ID churn;
selection changes are transactional; removed endpoints fail visibly and
recover without stale links, hangs or silent routing to a different device.
UI/CLI and daemon status agree on current endpoint and graph state.

**Required tests:** private PipeWire endpoint add/remove/rename/default-change,
same-name reappearance, server restart, selected-device switch during audio,
capture and playback independent-consumer checks, UI/CLI persistence and
sanitizer tests.

**Hardware/external validation:** real microphone and speaker unplug/replug or
equivalent device disable/enable on available hardware. Synthetic hotplug is
not a substitute for physical validation.

**Known limitations:** device-specific APO installation/registry behavior has
no literal Linux analogue; LinuxAPO follows PipeWire routing and permissions.

**Release readiness:** no unresolved route-loss/reconnect regression; publish
only with at least one documented physical endpoint-switch recovery run.

### v0.12.0 Beta: supported plugin workflow and compatibility

**Scope:** define and test the supported native plugin matrix (LV2, CLAP,
VST3); improve state, parameter, bypass, failure and latency/PDC UX. Decide
separately whether a native plugin GUI is required; do not claim one until it
actually loads and safely controls plugins. Keep FST/VST2 opt-in/experimental
unless compatibility and legal gates are met.

**Acceptance criteria:** per-format discover/load/parameter/state/bypass/reload
and latency behavior is documented; unsupported buses/features fail
actionably; saved state survives clean daemon restart; malformed state rolls
back to last valid graph; no host callback allocation; plugin failure does not
silently corrupt audio/config state.

**Required tests:** native fixture and selected real third-party plugins for
each claimed format, offline numeric output, PipeWire independent-consumer
E2E, restart/corrupt-state, dynamic PDC, block-size changes, callback
allocation and ASan/UBSan. Third-party coverage must name plugin/version.

**Hardware/external validation:** monitor/record at least one native plugin
chain through capture and playback on hardware. Wine/yabridge wrappers are
separately identified and never treated as native-plugin evidence.

**Known limitations:** plugin crashes/hangs remain in-process unless an
isolation milestone is explicitly implemented; automation curves, auxiliary
buses and GUI embedding may remain unsupported.

**Release readiness:** publish only the formats that meet the per-format gate;
exclude experimental VST2 from default packages and marketing claims.

### v0.13.0 Beta: EAPO config compatibility and analysis

**Scope:** prioritize high-value missing EAPO commands and semantics from
[`PORT_GAP_AUDIT.md`](PORT_GAP_AUDIT.md), particularly stages/device matching,
expressions/runtime variables, nested includes and channel/mix behavior; finish
useful magnitude/phase/latency response analysis. No Windows COM/Registry
replica.

**Acceptance criteria:** each claimed parser command has syntax/units/alias,
source-location diagnostics, runtime wiring and numerical or routing tests;
unsupported commands fail visibly; editor writes valid semantics and
round-trips untouched directives; response analysis has independent reference
fixtures and distinguishes modeled response from measured realtime latency.

**Required tests:** upstream-config corpus comparison; parser/error/Include
matrix; mono/stereo/multichannel numeric DSP and routing tests; UI
round-trips; response magnitude/phase fixtures; PipeWire consumer tests for
commands that affect routing/audio; renderer regression and sanitizers.

**Hardware/external validation:** sample-rate and channel-layout checks on
available physical devices. External configs/plugins require a documented,
permission-safe fixture corpus.

**Known limitations:** full Windows EAPO parity is neither assumed nor
required; unsupported Windows-specific commands remain explicitly listed.

**Release readiness:** no command may be called supported based only on parse
success; runtime effect and tests must be shown. Publish with an updated parity
audit and no silent command loss.

### v0.14.0 Beta: packaging, diagnostics and cross-distro hardening

**Scope:** stabilize supported package formats and upgrade/removal behavior,
systemd user lifecycle, diagnostics, docs, reproducible artifacts and
checksums; close reliability issues found across prior milestones.

**Acceptance criteria:** fresh install/upgrade/remove for every declared
package family; service start/restart/disable/remove semantics verified;
installed UI/CLI/docs/notices match source; packages build from a clean pinned
checkout; no generated/user plugin/cache material is bundled.

**Required tests:** clean build; CTest and sanitizer suites; package smoke and
transaction tests per declared distro; Nix flake check/build; artifact
checksums; startup/recovery and uninstall-no-leftover tests; hosted CI on exact
candidate commit.

**Hardware/external validation:** repeat capture and playback smoke after
install on the supported desktop. Other distros are only supported if their
package transaction was actually exercised.

**Known limitations:** distro versions beyond the tested matrix, unsupported
init systems, secure-boot/sandbox integration and third-party plugin sandboxing
are not implied.

**Release readiness:** package and repository state are clean, provenance and
licenses are documented, and all advertised artifacts pass their own smoke
tests.

### v1.0.0: stable release gate

**Scope:** freeze the supported hardware, PipeWire, config, plugin and
distribution matrix; resolve or explicitly exclude every mandatory gap; make
no new feature additions during the release candidate phase.

**Acceptance criteria:**

- System-wide playback and capture-to-virtual-mic meet their separate E2E and
  physical-device gates; switching/recovery is validated on real hardware.
- Supported EAPO syntax and plugin formats are explicitly defined; no
  supported command parses but is disconnected from realtime/offline output.
- No known critical crash, hang, data loss, unbounded memory or realtime
  allocation issue in supported use; last-valid-config rollback works.
- Full Release, ASan/UBSan, PipeWire, renderer/config, plugin, restart/recovery,
  PDC/latency, allocation and package suites pass on the final source commit.
- Clean install/upgrade/uninstall works for each declared distro/package;
  Nix check/build passes on the pinned source.
- GPL/upstream notices, plugin/SDK licensing and all redistributed artwork
  have release-specific clearance; no user-provided third-party plugin or
  yabridge/Wine payload is bundled.
- Independent user-facing documentation and final artifact hashes match the
  tagged commit.

**Hardware/external validation:** representative real capture and playback
devices, at least one physical switch/reconnect, third-party plugins for every
claimed plugin format, declared distro transaction matrix, and qualified
license review. Unavailable mandatory gates mean stop at the latest beta.

**Known limitations:** only the frozen support matrix is promised; literal
Windows APO/COM/Registry/service behavior is not a goal. Peace Equalizer
compatibility is not claimed without a dedicated integration test.

**Release readiness:** tag v1.0.0 only from a clean reviewed commit after every
mandatory criterion has attached evidence. Do not waive blockers by relabeling
them optional.

## Beyond 1.0 (optional)

Plugin subprocess isolation, automation curves, auxiliary plugin buses, wider
response/impulse analysis, additional package channels, broader hardware
certification and a potential Peace Equalizer backend integration are optional
until explicitly promoted into the stable support contract.
