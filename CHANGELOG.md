# Changelog

## Unreleased

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

## 0.1.0 — realtime prototype baseline

- Pinned official SourceForge Equalizer APO source as a clean submodule.
- Built actual upstream Preamp, BiQuad, IIR and Delay DSP for Linux.
- Added offline WAV rendering, native PipeWire device selection/capture/virtual source, daemon status, allocation checks and realtime numerical proof.
- This is a development baseline, not a stable 1.0 release.
