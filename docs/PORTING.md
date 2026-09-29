# Equalizer APO Linux port audit

Upstream is the official SourceForge repository `https://git.code.sf.net/p/equalizerapo/code`, cloned at `bbfcc3e5024cbb9d61ba75fc88d78605cc4c9687`, branch `main`, committed 2025-11-28. The checkout contains release tag 1.4.2 (`ca41d38`), dated 2025-03-21, but it is not an ancestor of this `main` history; `git describe` therefore finds no reachable release tag. The original Git history and GPL `License.txt` are retained under `upstream/equalizerapo`.

## Source findings

`IFilter` and `IFilterFactory` define a platform independent planar float processing interface. `PreampFilter` applies the upstream dB-to-linear gain implementation. `BiQuad` and `BiQuadFilter` contain the upstream filter equations/state and are suitable for Linux after replacing MSVC alignment/inlining syntax. `FilterConfiguration` owns preallocated channel workspaces and executes the filter chain; its DSP loop is portable, though its engine constructor depends on the Windows `FilterEngine` type.

`FilterEngine.cpp` is not portable as checked in: configuration discovery uses the registry; loading uses Win32 file APIs; reload uses Win32 events/semaphores/critical sections and `FindFirstChangeNotification`; it also depends on muParserX (`mpParser.h`), which is not vendored or declared as a git submodule in this checkout. Channel layout uses Windows `KsMedia.h` speaker masks. Those are platform abstraction/replacement work, not DSP code.

## Classification

| Classification | Inspected components | Findings |
| --- | --- | --- |
| Portable unchanged in principle | `IFilter.h`, `IFilterFactory.h`, Preamp/BiQuad/IIR/Delay/Channel/Copy filter implementations | The virtual filter API, DSP equations, channel selection and copy assignments are independent of Windows. `FilterConfiguration`'s processing loop is portable in principle, though this target currently uses a compatible adapter loop. |
| Small platform fixes | `BiQuad.h`, aligned allocation, `ChannelHelper` API | MSVC `__forceinline` / `__declspec(align)` and AERT allocator need POSIX/C++ equivalents; Windows speaker-mask lookup for the used Channel/Copy operations is supplied by a Linux implementation. |
| Platform abstraction required | `FilterEngine`, `MemoryHelper`, `ChannelHelper`, `LogHelper`, `StringHelper`, config path and watcher | Registry, Win32 I/O/sync, Windows codepages and speaker masks are used in implementations. |
| Windows integration replaced | `EqualizerAPO/`, `DeviceSelector/`, APO setup and registration | COM classes, APO installation, Windows audio engine and registry setup are outside the PipeWire backend. |
| Plugin host replacement | `VSTPluginFilter*`, `VSTPluginInstance/Library` | Existing implementation loads the Windows VST ABI and includes Windows file/memory-mapping assumptions; editor embeds Windows plugin GUI windows. Reuse chain concept only; native LV2/CLAP/VST hosts are not implemented yet. |

`GraphicEQFilter` derives from convolution; convolution uses upstream `libHybridConv`/FFTW and Windows-specific file handling. Loudness correction includes Windows endpoint-volume COM integration. These have not been enabled in the Linux target. The Windows VST host has not been forced into this build.

## Current Linux adapter scope

The current build compiles actual upstream `PreampFilter`, `BiQuad`, `BiQuadFilter`, `IIRFilter`, `DelayFilter`, `ChannelFilter`, `CopyFilter`, and their config factories. `Engine` is a Linux adapter that initializes and calls those actual `IFilter` implementations using preallocated channel buses and routing metadata. It is not the Windows `FilterEngine` class and does not claim full EAPO parser compatibility. It accepts `Preamp:`, `Filter:` (BiQuad and IIR forms), `Delay:`, `Channel:`, `Copy:` and relative/nested `Include:`. New Copy output channels are rejected because PipeWire ports are fixed at startup.

Memory uses `posix_memalign`; WAV I/O uses libsndfile. Native PipeWire enumeration, stable node-name selection, a capture-linked DSP filter/source, daemon supervision and status IPC are now implemented. The physical input passes through this existing adapter before reaching `SkyAPO Virtual Mic`. See `REALTIME.md` for hardware proof and audit boundaries. The complete upstream FilterEngine/parser port remains separate work; muParserX availability must be resolved before that can build.

## Clean upstream portability mechanism

`upstream/equalizerapo` is a Git submodule pinned to the revision above. Its `filters/BiQuad.h` is pristine. `cmake/PortableEapo.cmake` copies only the compiled filter translation units and headers into the build directory, then adapts the generated BiQuad header: MSVC `__declspec(align(16))` becomes standard `alignas(16)` and `<cfloat>` is included for the existing floating-point limit constants. `__forceinline` is mapped by the target compile definition. No DSP equations change.

The tiny generated copy is necessary because quoted includes resolve beside the upstream .cpp files; putting an alternative header earlier on the include path alone would not override them. It is regenerated at configure time, keeps copyright headers, and is never another vendored source tree. POSIX allocation and Windows helper compatibility remain external adapters. Review the patch when changing the submodule revision.

## Revision update procedure

Bootstrap with `git submodule update --init --recursive`. To update, fetch in `upstream/equalizerapo`, inspect and check out a reviewed exact revision, update these revision notes, rerun normal/sanitizer/hardware tests, then stage the new submodule gitlink in SkyAPO. Do not blindly pull an unreviewed revision or modify the official checkout. Root Git tracks a mode-160000 gitlink, not the upstream `.git` contents.
