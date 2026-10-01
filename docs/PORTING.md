# Equalizer APO Linux port audit

Upstream is the official SourceForge repository `https://git.code.sf.net/p/equalizerapo/code`, cloned at `bbfcc3e5024cbb9d61ba75fc88d78605cc4c9687`, branch `main`, committed 2025-11-28. The checkout contains release tag 1.4.2 (`ca41d38`), dated 2025-03-21, but it is not an ancestor of this `main` history; `git describe` therefore finds no reachable release tag. The original Git history and GPL `License.txt` are retained under `upstream/equalizerapo`.

## Source findings

`IFilter` and `IFilterFactory` define a platform independent planar float processing interface. `PreampFilter` applies the upstream dB-to-linear gain implementation. `BiQuad` and `BiQuadFilter` contain the upstream filter equations/state and are suitable for Linux after replacing MSVC alignment/inlining syntax. `FilterConfiguration` owns preallocated channel workspaces and executes the filter chain; its DSP loop is portable, though its engine constructor depends on the Windows `FilterEngine` type.

`FilterEngine.cpp` is not portable as checked in: configuration discovery uses the registry; loading uses Win32 file APIs; reload uses Win32 events/semaphores/critical sections and `FindFirstChangeNotification`; it also depends on muParserX (`mpParser.h`). Channel layout uses Windows `KsMedia.h` speaker masks. Those are platform abstraction/replacement work, not DSP code.

### Legacy MuParserX dependency investigation

The [upstream Equalizer APO developer wiki](https://sourceforge.net/p/equalizerapo/wiki/Developer%20documentation/) links the `muparserx_v3_0_1.zip` attachment and explicitly requires version 3.0.1 because semicolon sequencing was removed in 3.0.2. SkyAPO retrieved the official attachment on 2026-09-30; its original SHA-256 is `11689042843844638414af070f928ef5200461dc59fa237bad4a01531f982609`, and its `License.txt` declares BSD-2-Clause. For the public source tree, `packaging/muparserx_v3_0_1.zip` is a source-only repack of that verified attachment: it preserves every `parser/` source/header and the identical license text, while excluding unused MSVC `.lib`/`.pdb` binaries, build outputs and documentation assets. The repack SHA-256 is `ee6927c135182681d331f219c3f4b991f29cfd4ecf5ae33efff78b0533e9c59e`; CMake verifies this exact hash before extraction. A local Linux build of the parser sources succeeded with GCC 16. The upstream archive's Makefile omits `mpParserMessageProvider.cpp`, so its sample link fails unless that source is added explicitly. A standalone API check linked the parser with that source and verified that evaluating `1; 2` returns `2`, demonstrating the required legacy semicolon behavior.

The archive is now integrated reproducibly as the default conditional-expression backend: CMake verifies the pinned SHA-256, compiles the upstream parser sources (not the bundled Windows binaries), and installs the BSD-2-Clause notice. The production `If:`/`ElseIf:` path tests legacy semicolon sequencing and reports parser failures with config path/line. Classic muParser remains an explicit optional fallback, but is not claimed API- or syntax-compatible with MuParserX. This does not complete the upstream `FilterEngine` port: its Registry and Win32 synchronization/I/O remain separate Linux-adapter work. SkyAPO's Linux `ConfigWatcher` is implemented outside upstream `FilterEngine` and watches the root config plus active recursive Include dependencies.

## Classification

| Classification | Inspected components | Findings |
| --- | --- | --- |
| Portable unchanged in principle | `IFilter.h`, `IFilterFactory.h`, Preamp/BiQuad/IIR/Delay/Channel/Copy filter implementations | The virtual filter API, DSP equations, channel selection and copy assignments are independent of Windows. `FilterConfiguration`'s processing loop is portable in principle, though this target currently uses a compatible adapter loop. |
| Small platform fixes | `BiQuad.h`, aligned allocation, `ChannelHelper` API | MSVC `__forceinline` / `__declspec(align)` and AERT allocator need POSIX/C++ equivalents; Windows speaker-mask lookup for the used Channel/Copy operations is supplied by a Linux implementation. |
| Platform abstraction required | `FilterEngine`, `MemoryHelper`, `ChannelHelper`, `LogHelper`, `StringHelper`, config path and watcher | Registry, Win32 I/O/sync, Windows codepages and speaker masks are used in implementations. |
| Windows integration replaced | `EqualizerAPO/`, `DeviceSelector/`, APO setup and registration | COM classes, APO installation, Windows audio engine and registry setup are outside the PipeWire backend. |
| Plugin host replacement | `VSTPluginFilter*`, `VSTPluginInstance/Library` | Existing Equalizer APO implementation loads the Windows VST ABI and includes Windows file/memory-mapping assumptions; editor embeds Windows plugin GUI windows. Reuse chain concept only. SkyAPO has separate native LV2, CLAP, VST3 and opt-in FST-backed VST2 hosts; the opt-in VST2 path accepts restricted `VSTPlugin:` syntax for Linux-loadable modules, not Windows DLLs or legacy inline `ChunkData`. It fixture-tests native program-chunk sidecars for effects advertising `ProgramChunks`; third-party behavior remains unverified. Plugin licensing, isolation and broad third-party compatibility remain open. |

`GraphicEQFilter` derives from convolution; convolution uses upstream `libHybridConv`/FFTW and Windows-specific file handling. Linux enables the actual upstream GraphicEQ filter, GainIterator and libHybridConv implementation when FFTW3f is present, and supplies a Linux convolution subclass that reads IR files with libsndfile. It resolves relative IR paths against the config file and rejects empty, unreadable, or rate-mismatched IRs. Convolution requires an exact fixed block size: the daemon waits until PipeWire negotiates its graph quantum, builds the filter off the callback, and rebuilds outside the callback if rate/quantum changes. The offline renderer zero-pads its final partial block. `LoudnessCorrectionFilter` and its factory are also the upstream implementation; only the Windows `VolumeController` integration is replaced by a Linux snapshot provider. The Windows VST host has not been forced into this build.

### `FilterConfiguration` constructor boundary

The official submodule remains unchanged. CMake adapts only generated
build-tree copies: `FilterConfiguration.h/.cpp` and generated
`IFilterFactory.h` both consume the Linux-neutral `IFilterFactoryContext`,
which exposes only real/output channel counts and maximum frame count. The
Linux runtime supplies a short-lived `FilterConfigurationContext`; a future
upstream `FilterEngine` adapter can implement the same contract directly.
The old global-name Linux `FilterEngine` shim has been removed, avoiding
confusion and ODR/API collisions with upstream's Windows runtime class.
Replacement tokens are checked during CMake configuration so upstream drift
fails loudly. The context is valid only during factory initialization;
factories must not retain it. This is an incremental seam, not a port of
upstream `FilterEngine.cpp`, which still requires replacement of Win32
threading, file I/O, registry and APO services. The Linux daemon owns config
lifecycle and uses a separate `ConfigWatcher` for inotify-based root/Include
tracking. `src/platform/linux/ConfigSource` now owns Linux path canonicalization,
relative Include resolution and streaming line reads; directive semantics,
factory lifecycle and candidate-graph transactions remain in `Engine`. Its
focused tests preserve absolute/relative path behavior, line boundaries, EOF
and missing-file diagnostics. Together with XDG `Settings`, the inotify
`ConfigWatcher`, and the PipeWire control-loop candidate Engine publication,
these are the Linux replacements for the upstream engine's Registry config
discovery, Win32 file reads, directory-notification thread, semaphore and
critical-section ownership path. The upstream Windows loader is deliberately
not compiled: its watcher waits on the audio-side transition semaphore and its
Registry/APO services have no Linux equivalent. The Linux control loop keeps
the already-tested `Engine` factory lifecycle and graph transaction while the
audio callback only processes installed Engines and reports transition
completion. Coverage is split across `skyapo-config-source`,
`skyapo-config-watcher`, `skyapo-upstream-process`, upstream config fixtures,
and private PipeWire Include-reload/rollback E2E; this proves the replacement
services, not source-level portability of upstream `FilterEngine.cpp`.

## Current Linux adapter scope

The current build compiles upstream `FilterConfiguration.cpp` and uses its actual `read` → `process` → `write` path with actual `PreampFilter`, `BiQuad`, `BiQuadFilter`, `IIRFilter`, `DelayFilter`, `ChannelFilter`, `CopyFilter`, and their factories. The supported factories are initialized and driven in the same ordered `IFilterFactory` lifecycle as upstream: configuration start/end, recursive file start/end, and command dispatch that stops at the first produced filter. `FilterConfigurationContext` and `IFilterFactoryContext` expose only the three sizing values needed by the upstream buffer owner and selected factory API. With FFTW3f available it also compiles upstream `GraphicEQFilter`, `GainIterator`, `ConvolutionFilter` and `libHybridConv`; Linux overrides convolution IR loading while retaining the upstream partitioned-convolution DSP. The actual upstream `LoudnessCorrectionFilter`/factory are included for daemon processing. Its control worker reads an atomic snapshot of uniform, positive PipeWire default-render channel gain; no PipeWire types enter the filter. Offline processing/config validation reject it when no live provider is active. The outer `Engine` remains a Linux config parser/graph builder and does not claim full EAPO parser compatibility. It accepts `Preamp:`, `Filter:` (BiQuad and IIR forms), `Delay:`, `Channel:`, `Copy:`, `GraphicEQ:`, `Convolution:`, daemon-only `LoudnessCorrection:`, relative/nested `Include:`, and (when Lilv is available) `Plugin: LV2` with validated input-control overrides. Copy-created intermediate channels use preallocated upstream FilterConfiguration scratch planes and can be mixed back into the fixed PipeWire output layout.

`skyapo-upstream-config-compatibility` pins explicit outcomes for the official `Setup/config` samples: `iir_lowpass.txt` and `selective_delay.txt` parse through the production command/factory path; `example.txt` and `demo.txt` are Room EQ Wizard `Room EQ V5` exports rather than native EAPO syntax, so they are rejected at line 1. Upstream `config.txt` and `multichannel.txt` include those external-format exports and are likewise rejected at the included source line. This is intentional compatibility evidence, not a claim that all files shipped in the upstream setup folder are Equalizer APO configs.

The metadata boundary now has one shared generated `IFilterFactoryContext` implemented by `FilterConfigurationContext`; both selected factories and upstream `FilterConfiguration` consume it, and a Linux-portable upstream `FilterEngine` can implement it without a second context ABI. This removes the direct type-name/ODR collision but does not make upstream `FilterEngine.cpp` portable. `Engine::processTransitionTo()` reuses the real upstream `FilterConfiguration::doTransition()` cosine mix for two prebuilt compatible configurations. For same-format config reloads, `Runtime` publishes a control-owned pending Engine; the PipeWire callback processes both graphs for the upstream 10 ms transition without allocating, publishes completion, and the control loop promotes ownership/retires the old graph only after callback quiescence. `audioMode` gates callbacks during control-thread CLAP state saves; sample-rate/quantum changes are hard rebuilds rather than crossfades between incompatible workspaces. Unit/realtime-allocation tests cover variable blocks crossing the 480-frame 48 kHz boundary, and private PipeWire Include-reload plus CLAP-state tests verify the live path. This still does not make upstream `FilterEngine.cpp` portable: its config discovery, watcher, and Windows semaphore flow need separate Linux adapters, and the upstream publication flow must not be copied into the callback unchanged.

Conditional flow supports nested upstream-style `If:`, `ElseIf:`, `Else:` and `EndIf:` with MuParserX 3.0.1 by default and classic muParser as an optional fallback. In the MuParserX build, the same parser instance spans the root config and nested Includes, defines `sampleRate`, `inputChannelCount` and `outputChannelCount`, evaluates `Eval:` expressions, and expands inline backtick expressions in active commands. Portable upstream `regexSearch` and `regexReplace` callbacks are adapted from `parser/RegexFunctions.cpp` and run outside realtime processing. The production parser now also compiles upstream `LogicalOperators` and `StringOperators` build-tree adaptations, including `not` and string-aware `+`; upstream files in the SourceForge submodule remain unchanged. Tests cover semicolon sequencing, a variable assigned by `Eval:` and consumed in an included file, numeric inline gain expansion, both regex functions, upstream string concatenation/`not`, source locations on invalid expressions, and last-valid-graph preservation. The unmodified upstream `Setup/config/iir_lowpass.txt` also passes the production parser/factory/DSP path with measured passband and stopband attenuation. Classic muParser builds explicitly reject `Eval:` and inline expressions. This is still a subset: Windows registry functions, other upstream operator overrides, and full string/matrix expression semantics are not implemented.

Memory uses `posix_memalign`; WAV I/O uses libsndfile. Native PipeWire enumeration, stable node-name selection, a capture-linked DSP filter/source, daemon supervision and status IPC are now implemented. The physical input passes through this existing adapter before reaching `SkyAPO Virtual Mic`. See `REALTIME.md` for hardware proof and audit boundaries. The complete upstream FilterEngine/parser port remains separate work: its Win32/Registry services and additional parser factories still require Linux adapters. The current Linux config watcher is external to the upstream engine and has tested recursive Include reload/rollback, recovery after an included directory is removed/recreated, recovery when a newly referenced missing Include file is created, missing parent-directory creation, and inotify queue-overflow handling.

## Clean upstream portability mechanism

`upstream/equalizerapo` is a Git submodule pinned to the revision above. Its `filters/BiQuad.h` is pristine. `cmake/PortableEapo.cmake` copies only the compiled filter translation units and headers into the build directory, then adapts the generated BiQuad header: MSVC `__declspec(align(16))` becomes standard `alignas(16)`, `<cfloat>` is included for the floating-point limit constants, and the denormal macro uses `std::abs`. The latter matters because unqualified `abs(double)` resolved to an integer overload in the Linux build (`abs(0.5) == 0`), causing `BiQuadFilter::removeDenormals()` to erase ordinary filter state at block boundaries and distort frequency response. A regression compares the actual block-processed upstream filter output against its steady-state gain. `__forceinline` is mapped by the target compile definition. For GraphicEQ/Convolution the generated copy also removes Windows-only includes and an optional FFTW threads-helper call; GraphicEQ's upstream scalar `delete` for an array buffer is corrected to `delete[]`. The core DSP equations remain upstream implementations.

The tiny generated copy is necessary because quoted includes resolve beside the upstream .cpp files; putting an alternative header earlier on the include path alone would not override them. It is regenerated at configure time, keeps copyright headers, and is never another vendored source tree. POSIX allocation and Windows helper compatibility remain external adapters. Review the patch when changing the submodule revision.

The same build-only adaptation now covers upstream `LoudnessCorrectionFilter`: its COM `VolumeController` is replaced with the PipeWire-backed `LoudnessVolumeProvider`; Win32 thread/event calls map to `std::thread` and atomic events in `EapoSyncCompat.h`. The filter's realtime callback only polls atomics and uses `try_lock` to copy worker-generated coefficients; if the control worker holds the lock, the event remains pending for the next callback. The worker may block on the mutex, but the audio thread never does. `_attFactor` is atomic because the upstream worker updates it while the audio thread multiplies samples. Worker creation is checked and handles are initialized, zero-difference `preAmp` is initialized (the upstream branch otherwise leaves it indeterminate), and the worker always refreshes on the first available Linux volume sample. `ParameterArchive.h` is copied and adapted for UTF-8/POSIX; its Windows `CP_ACP`/`CopyMemory` calls and an upstream byte-count bug (`sizeof(std::wstring)` rather than `sizeof(wchar_t)`) cannot be used on Linux. Strict configure-time checks fail if expected upstream patch sites disappear. The selected filter sources, factory and GPL notices remain in the submodule; these changes are generated under `build/eapo-port` only.

The Linux config loader also ports the actual matching behavior of
`filters/DeviceFilterFactory::matchDevice` without instantiating the Windows
factory: semicolon-separated OR groups, whitespace-separated AND terms,
case-insensitive substring matching, `all`, and the upstream GUID handling.
Its Linux match string is built from stable `node.name`, description, and
available serial/bus/ALSA identity properties; PipeWire's transient numeric
node ID is never included. The daemon passes this context into each fresh
Engine on initial load, format rebuild, and config reload. Config validation
uses only the selected stable node name so it remains independent of a live
PipeWire registry connection; offline render accepts an explicit `--device`
stable name. Description/serial-only matches may therefore differ in config-
check filter counts from daemon runtime.
The matcher keeps its upstream GPL attribution and is covered by direct DSP
tests plus a private PipeWire E2E that verifies the processed virtual-mic audio.

## Revision update procedure

Bootstrap with `git submodule update --init --recursive`. To update, fetch in `upstream/equalizerapo`, inspect and check out a reviewed exact revision, update these revision notes, rerun normal/sanitizer/hardware tests, then stage the new submodule gitlink in SkyAPO. Do not blindly pull an unreviewed revision or modify the official checkout. Root Git tracks a mode-160000 gitlink, not the upstream `.git` contents.
