# Changelog

## Unreleased

- Added pinned official Steinberg VST3 SDK components, a Linux hosting subset, cached bundle discovery through standard paths and `VST3_PATH`, and CLI inspection.
- Added initial mono/stereo, single-audio-bus VST3 processing; manually rendered the installed LSP Filter Stereo plugin offline (192000 frames at 48 kHz; output samples changed).
- Added a SkyAPO-authored VST3 half-gain bundle fixture; CTest measures −6.0206 dB offline and audits 1000 variable realtime processing blocks without host/DSP callback allocations.
- Added VST3 writable parameter metadata and numeric-ID normalized config overrides; CTest validates metadata, −12.0412 dB override output, invalid ID/range rejection and allocation-free callback processing with a static override.
- Verified offline processing of a local yabridge ATKExpander VST3 wrapper in mono (192000 frames at 48 kHz; samples changed). Its stereo use was correctly rejected because the plugin exposes a mono bus.
- Documented VST3 limits: live automation, auxiliary buses, state, latency compensation, UI and isolation remain unsupported; yabridge realtime and VST2 remain unverified.
- Audited Steinberg's current VST2 licensing terms; documented the pre-October-2018 license requirement and kept VST2 open as an external legal blocker rather than vendoring restricted headers.
- Added a pinned official CLAP SDK submodule and initial native CLAP effect host with CLAP_PATH/standard-path discovery, config IDs, offline render and 1000-block realtime-allocation tests.
- Added a native CLAP stereo half-gain test plugin; CTest confirms the expected −6.0206 dB output and no recurring host-side callback allocation. Live events/state/latency and production plugin safety remain unsupported.
- Added CLAP parameter metadata inspection and config-time overrides by stable numeric ID or unique name; validated range/unknown-value rejection and numerical offline gain. Fixed overrides are delivered using preallocated process events.
- Added physical input channel counts and published sample-rate metadata to `skyapo device list`; the GUI selector displays known values and leaves unavailable rates unknown.
- Fixed config comment stripping so `#` inside quoted Include paths is preserved; added nested-file regression coverage.
- Added row focus/selection, Delete removal, and Alt+Up/Alt+Down config reordering to the Linux-hosted upstream filter rows.
- Added a Linux Include editor with config-relative path validation and a native file picker.
- Added a Qt 6 `skyapo-ui` editor that reuses upstream Preamp/BiQuad/Delay/Stage GUI widgets, preserves untouched config bytes, and drives daemon/device/config actions through the existing CLI.
- Made editor CLI actions asynchronous and coalesced periodic status/device queries so stalled daemon/device requests do not freeze the Qt event loop.
- Reused upstream `FilterTableRow` and its Qt resources through a narrow Linux compatibility shim that excludes Windows device/registry table behavior.
- Audited upstream Qt Editor reuse boundaries and documented which Windows device/APO integrations must become SkyAPO daemon clients.
- Added LV2 input-control metadata inspection (`skyapo plugin info`) and validated config-time parameter overrides using `symbol=value` syntax.
- Added an initial native LV2 host and non-realtime plugin discovery CLI; audio/control-port plugin processing is tested offline and under callback allocation audit.
- Moved directive dispatch and factory lifecycle calls onto the upstream `IFilterFactory` ordering; Linux-only Stage/Include handling remains around the factory chain.
- Added `skyapo filters` and `skyapo diagnostics`, reporting the daemon's active directive/source lines and compiled upstream revision.
- Added a PipeWire-ordered systemd user service, CMake install rules, and an Arch PKGBUILD that builds the pinned SourceForge submodule.
- Built the Arch package from a fresh local-source clone; its submodule initialized at the pinned commit and packaged CTest passed 3/3.
- Added CLI daemon start/stop/restart and config show/reload over the private Unix socket; CLI-launched daemon detaches and logs to the user's config directory.
- Made `skyapo start` wait for negotiated sample rate/quantum instead of returning while the DSP graph is still uninitialized.
- Added capture-stage selection and cleanly skipped Windows pre/post-mix stages; nested Includes inherit stage context without mutating their parent.
- Replaced SkyAPO's local channel-bus execution loop with upstream `FilterConfiguration` read/process/write; added a narrow Linux metadata shim for its portable constructor.
- Revalidated normal and ASan/UBSan PipeWire recording through the upstream `FilterConfiguration`: 192000 captured frames, correlation 1, measured -6 dB ratio 0.501187.
- Added optional upstream GraphicEQ/Convolution and libHybridConv build support when FFTW3f is present.
- Added Linux libsndfile impulse-response loading with config-relative paths and validation.
- Deferred daemon DSP graph construction until actual PipeWire rate/quantum negotiation, so fixed-block convolution never rebuilds in the audio callback.
- Made offline rendering pad the last partial convolution block and added impulse-response/fixed-block tests; GraphicEQ executes in the core tests.
- Added a 1000-block upstream convolution realtime-safety audit; normal and ASan/UBSan CTest remain green.
- Extended the generated upstream compatibility patch for Windows headers, optional FFTW thread-helper calls, and GraphicEQ's mismatched array deallocation.
- Added portable recursive `Include:` expansion with relative paths, quoted paths, nesting/cycle guards, and file/line errors.
- Config loads remain transactional: a parse or include failure does not replace the active filter list.
- Added debounced inotify reload; candidate graphs are built on the control loop, atomically activated, and old graphs are reclaimed only after audio callbacks leave them.
- Added nested-include, sample-output, error-location, cycle, and last-valid-config tests.
- Reused upstream ChannelFilter/CopyFilter and factories with Linux channel-name compatibility and preallocated channel routing; unknown Copy sources and output layouts outside the fixed PipeWire ports fail with source/line errors.
- Expanded channel-selection and Copy-swap sample tests plus variable-block allocation tests.
- Revalidated the unchanged realtime -6 dB path after channel-routing refactor: 192000 captured frames, correlation 1, ratio 0.501187, zero audited callback allocations/deallocations.
- Repeated the real PipeWire recording with both normal and ASan/UBSan daemons after routing changes; both captured 192000 frames at the expected -6 dB.
- Added the staged 0.2.0 through 1.0.0 roadmap and acceptance criteria.
- Advertise PipeWire F32P formats at 44.1, 48 and 96 kHz; verify physical-to-virtual stereo recordings and −6 dB gain at every rate.

## 0.1.0 — realtime prototype baseline

- Pinned official SourceForge Equalizer APO source as a clean submodule.
- Built actual upstream Preamp, BiQuad, IIR and Delay DSP for Linux.
- Added offline WAV rendering, native PipeWire device selection/capture/virtual source, daemon status, allocation checks and realtime numerical proof.
- This is a development baseline, not a stable 1.0 release.
