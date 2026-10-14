# Equalizer APO → LinuxAPO: source-level port gap audit

Audit date: 2026-10-02
Scoring/classification re-review: 2026-10-14
LinuxAPO: `b656971ef73b15dceb21dac105b23687f231fed3` (`main`, after the public `v0.9.0` tag)
Equalizer APO: submodule `bbfcc3e5024cbb9d61ba75fc88d78605cc4c9687` (`upstream/equalizerapo`)
Scope: source/runtime/UI parity audit. LinuxAPO has uncommitted implementation changes in this worktree; this review reads current files without changing implementation. The upstream comparison remains pinned to the stated gitlink. A prior correction for EAPO inactive filter-slot syntax is present in the current working tree and documented below. This document was subsequently refreshed for the editor's tested saved-parameter dialog; the source-status caveats below reflect that current worktree. No release version, tag, or remote was changed.

This audit uses **PASS** for actual wired behavior, not similarity of names or source files. LinuxAPO deliberately consumes selected upstream EAPO filters and `FilterConfiguration`; it does not compile or run the complete Windows `FilterEngine`/COM APO stack. “Upstream” below means the pinned submodule revision above. Evidence paths are relative to repository root unless prefixed `upstream/equalizerapo/`.

## Executive result

### Scoring/classification correction on 2026-10-14

The old 97-row ledger is not present in this repository or its reachable Git history, so its uniqueness cannot be verified. The prior matrix had 106 rows after separating plugin insertion, parameter UI, state UI, and native plugin-editor hosting, and adding plugin-inclusive response analysis. A case-insensitive exact-label check found all 106 labels unique; semantic independence is a separate question. This re-review removes three overlapping rows: the unsupported standalone “plugin state editor UI” atom (state interaction is covered by upstream plugin GUI hosting), a duplicate Windows Device Selector install/enable row, and a summary Windows APO-host row already represented by separate capture, format, lifecycle and stage capabilities. It also changes the aggregate Stage editor atom from MISSING to PARTIAL (capture is selectable; pre/post are disabled). The corrected matrix had 103 unique labels; adding analysis-source selection produced 104. The current 105-row matrix additionally lists FFT resolution separately; its implementation is partial, as the Linux UI supports only a subset of upstream sizes. Other corrections retained from the earlier review are runtime plugin state `BACKEND_ONLY → PASS`, plugin latency/PDC `BACKEND_ONLY → PASS`, device selection `REPLACED → PARTIAL`, and the specific pre/post Stage controls remain MISSING. Generic service supervision remains excluded from device scoring; plugin-inclusive response analysis remains MISSING. The original 97 labels remain unavailable for verification.

LinuxAPO is a substantial capture-processing port, not a drop-in port of all EAPO. It reuses actual upstream DSP filter implementations and GUI widgets, while replacing device activation/runtime integration with a per-user PipeWire graph. The main parity gaps are the Windows playback/pre-mix stages, several EAPO parser variables and legacy Registry functions, full multi-channel interoperability, upstream VST GUI integration, many editor workflows, and full response-analysis tools.

The current 105 row labels are textually unique (case-insensitive); this does **not** make them 105 independent user capabilities. The former 97-row snapshot is not retained in this file or Git history, so its labels cannot now be independently rechecked. The list deliberately separates some implementation layers, while other rows group multiple upstream operations. Treat matrix counts as classified audit entries, not deduplicated features or an implementation score. Domain scorecards overlap intentionally and do not sum to the matrix-entry count. The basic `DeviceFilterEditor` has a focused Qt edit/save assertion; that proves pattern round-trip, not endpoint-selector parity.

| Classification | Rows |
|---|---:|
| PASS | 38 |
| PARTIAL | 41 |
| BACKEND_ONLY | 3 |
| UI_ONLY | 0 |
| BROKEN | 0 |
| MISSING | 11 |
| REPLACED | 6 |
| N/A_WINDOWS | 6 |
| **Total reviewed matrix entries** | **105** |

### Corrections from the prior scorecard

| Item | Original audit | Corrected audit | Why |
|---|---|---|---|
| Runtime plugin state persistence | BACKEND_ONLY | PASS | Host source implements state save/restore for all four formats; editor visibility is a different capability. Fixture round-trip tests cover all four formats; arbitrary third-party state behavior remains unproven. |
| Runtime plugin latency/PDC | BACKEND_ONLY | PASS | Engine/plugin source implements latency reporting, PDC, refresh and rebuild. Lack of a UI panel is irrelevant to runtime correctness. |
| Plugin UI row | One PARTIAL row | Four scored capabilities: native insertion PASS; VST2 insertion MISSING; parameter editor PARTIAL; native plugin GUI MISSING | Former row bundled separate EAPO workflows and partly counted insertion again in backend rows. “Plugin state editor UI” was removed as a separate atom: upstream has no host-owned state browser/editor distinct from its plugin GUI, and Linux’s missing GUI hosting already captures that gap. Runtime state persistence remains separately scored. |
| Plugin parameter editor UI | MISSING in audit snapshot | PARTIAL in current worktree | Since the audit-only snapshot, Tools gained a format-checked config-value editor for LV2/CLAP/VST3. UI tests exercise each format's accepted key grammar and byte-preserving save. Live values, state editing and VST2 UI remain absent. |
| Plugin-inclusive response analysis | Not enumerated | MISSING, one atom | Upstream analysis runs the configured VST filter where usable; Linux explicitly disables plugin loading in `ResponseAnalysis`. |
| Device selection parity | REPLACED | PARTIAL | Linux selects one capture input; EAPO Device Selector covers endpoint installation/configuration across render and capture endpoints. Current working-tree selection persists `device.serial` or `device.bus-id` when advertised and resolves by it, with `node.name` fallback; many devices expose neither property and rename/hardware coverage remains unverified. |
| Device-management service supervision | Included as PARTIAL atom | Removed from denominator | It is daemon/service integration, not Device Selector behavior; service supervision remains in runtime and packaging scorecards. |
| Stage editor aggregate | MISSING | PARTIAL | The editor can still select/view the capture stage; pre/post controls are disabled. The specifically missing pre/post selection remains a MISSING sub-capability, but the aggregate Stage editor is not wholly missing. |
| Device-management denominator | 8.5/13 | 7.5/12 = 62.5%, then source re-review 7/12 = 58.3% | The prior ledger counted daemon restart/reload as a device-management atom even though it is daemon lifecycle/runtime behavior and is already represented in that scorecard. Stable identity remains PARTIAL because only advertised serial/bus-id values survive a node rename; devices without those properties use node-name fallback. |
| Stable device persistence | REPLACED | PARTIAL | `Settings::select()` stores a second-line stable property; `Runtime::init()`/`selectionChanged()` resolve against it, falling back to `node.name` for legacy or identity-less selections. A private PipeWire E2E now recreates a source under a new name with the same `device.serial` and verifies daemon recovery; physical-device/vendor coverage remains unverified, and many devices publish no serial/bus-id. This is implemented and fixture-tested but incompletely portable, not MISSING. |
| Native plugin instance insertion in editor | PARTIAL | PASS | The scoped row covers insertion for all exposed native formats (LV2/CLAP/VST3), each selected through the installed-plugin picker and serialized as a config directive. FST/VST2 insertion remains a separate MISSING row; UI tests cover native picker insertion. |
| LV2 plugin state test evidence | No LV2 round-trip test found | Round-trip test exists | `tests/lv2_live_control_test.cpp` saves state, creates a new Engine and verifies restored output. This corrects evidence wording only, not the PASS classification or score. |
| Add Filter omissions | Claimed Device template missing | Corrected: `Device:` match template exists; an `Include:` template was added after this audit review | `DeviceFilterEditor` and `populateAddPopupMenu()` provide a basic match editor; it is not endpoint-selector parity. The Include template is verified by `skyapo-ui-editor`; broader missing template categories remain. |
| Crash-supervision statement | “No guaranteed supervisor” | Qualified | Supplied systemd user unit and SIGKILL E2E restart are present; manually launched daemon or disabled unit is not supervised. |
| Matrix size | 101 labels | 103 labels after atomization and duplicate removal | Added plugin-response analysis; removed plugin-state UI, duplicate Windows selector-enabling, and summary Windows APO-host rows where their capability is already represented. The old 97-label snapshot cannot be recovered. |

The prior audit reproduced a config compatibility defect for upstream inactive filter slots. It is fixed in the current working tree: Linux now treats `OFF` slots (including retained filter parameters) and `ON None` as valid no-ops, matching the upstream factory behavior; `tests/core_tests.cpp` covers all three forms numerically. The fix does not alter DSP code. The current matrix therefore has no evidenced BROKEN row; that does not prove there are no other defects.

## Evidence and method

Inspected the pinned `FilterEngine.cpp/.h`, `FilterConfiguration.cpp/.h`, `IFilter.cpp/.h`, all `filters/*Factory.cpp` and DSP implementation files, parser helpers, Device Selector/Configurator sources and forms, Editor C++/forms/translations/analysis files, Benchmark, Setup/installer, and the Linux `Engine`, PipeWire `Runtime`, CLI, daemon, editor, plugin hosts, tests and CMake source wiring. Also searched for Win32/COM/Registry/file-watcher/device terms in both trees.

The 2026-10-14 scoring/classification re-review did not rerun the broad suite. The following are previously recorded test results and source evidence, not fresh verification of the whole dirty worktree. Earlier audit-time command:

```text
ctest --test-dir build --output-on-failure -R '^(skyapo-core|skyapo-pipewire-e2e-stereo-48000)$' --parallel 1
100% tests passed, 0 tests failed out of 2
```

`skyapo-core` numerically asserts `Preamp: -6 dB` against `10^(-6/20)` within `1e-5`; the private PipeWire E2E test asserts a steady-state independent-consumer signal and runtime ratio `0.501187`. Source: `tests/core_tests.cpp` and `tests/pipewire_e2e_test.py` (stereo/48 kHz fixture). These are not evidence of this audit session recording from a physical device.

After the scoring review, response-analysis source selection and a limited resolution control were implemented. Current focused evidence: `cmake --build build -j2 --target skyapo-ui skyapo-ui-tests` PASS; `ctest --test-dir build --output-on-failure --repeat until-fail:2 -R '^skyapo-ui-editor$'` PASS 2/2. The UI test numerically verifies −6 dB from saved default `config.txt`, −3 dB from the unsaved current editor document, 8192-sample FFT selection producing 4096 bins, and resolution preference persistence. The dialog is labelled calculated, not measured, because it is theoretical impulse-response analysis. This focused result does not refresh the unrelated historical results or constitute a full regression run.

Status means implementation state, not test coverage: absent a test is not by itself MISSING or PARTIAL where source and runtime wiring demonstrate the behavior. Conversely, source presence alone does not earn PASS if the runtime does not execute the behavior or source inspection proves a limitation. Test confidence is stated separately in the evidence/caveat columns and the test-gap section.

### Row uniqueness and granularity check

The former 97 labels cannot be rechecked because that snapshot is absent. The current 105 labels are case-insensitively unique. They are not all independent capabilities: filter directive syntax vs subtype implementation, capture processing vs virtual-source consumption, stable selection vs editor selector, and plugin backend/config/control vs UI exposure are separate layers. A layer is counted once within a domain; one workflow can appear in separate domains when each measures a different layer. Within the plugin score, backend hosting and config-command wiring remain separate atoms because both must independently work; editor insertion is no longer counted in either backend atom. Response-analysis computation and editor presentation are likewise separate domains. Native plugin parameter editing is distinct from host GUI embedding; a separate state-editor atom was removed because that duplicates GUI/state interaction not provided as a separate EAPO host capability. Pre-mix and post-mix remain separate because upstream has distinct placements. Generic user-service supervision is not a device-management capability and is excluded from that denominator.

The opposite granularity issue remains: `Input/output device/default distinction and metadata` groups several Device Selector capabilities; Include resolution combines path, quoting, nesting and cycle/depth behavior; and some biquad aliases/types are clustered. The scorecards atomize some groups (for example, eight biquad types), but the master matrix is not a one-row-per-atomic-upstream-feature inventory. “105 rows” is a row count, not proof that all upstream capabilities are exhaustively atomized.

## Master parity matrix

### Core, parser, filters, configuration

| Upstream capability | Status | Upstream evidence | LinuxAPO evidence / tests | Difference or caveat |
|---|---|---|---|---|
| `IFilter` lifecycle and factory contract | PASS | `IFilter.*`, `IFilterFactory.h` | Same source compiled through `CMakeLists.txt`, factories registered in `src/core/Engine.cpp` | Reused interface and implementations. |
| `FilterConfiguration` channel read/process/write | PASS | `FilterConfiguration.*` | Upstream class created by `Engine::loadConfig`; process adapter in `src/platform/linux/UpstreamFilterEngineProcess.h`; `skyapo-core`, `skyapo-offline-render` | Linux constructs channel metadata/context externally; this is not the original full engine graph loader. |
| Ordered filter initialization/processing | PASS | `FilterEngine::initializeFilters`, `FilterConfiguration::process` | `Engine::loadConfig`, `FilterConfiguration`; `skyapo-core` | Linux configuration builder handles its supported filter nodes and extra routing/PDC wrappers. |
| Full EAPO `FilterEngine` lifecycle/config loader | REPLACED | `FilterEngine::initialize`, `loadConfig`, `loadConfiguration`, `process` | `src/core/Engine.cpp`, `Runtime.cpp`, `ConfigWatcher.*` | Replaced with an explicit Linux parser/graph builder and daemon. This is not behaviorally identical for all parser factories, pipeline stages, variables or device APIs. |
| `Preamp` dB gain | PASS | `filters/PreampFilter{,Factory}.cpp` | Upstream factory/filter; `tests/core_tests.cpp`, renderer tests and PipeWire E2E | Upstream multiplication `10^(dB/20)` retained. |
| `Filter: ON` biquad syntax | PASS | `BiQuadFilterFactory.cpp`, `BiQuadFilter.cpp` | Upstream factory/filter; `tests/core_tests.cpp`, UI tests | Linux editor covers the common biquad fields. Exact legacy permissiveness follows upstream factory, subject to active Engine validation. |
| Biquad `PK`/`PEQ`/`Modal` | PASS | `BiQuadFilterFactory.cpp` | Upstream source compiled; core/UI/response tests | Aliases map to peaking. |
| Biquad `LP`/`LPQ`, `HP`/`HPQ` | PASS | `BiQuadFilterFactory.cpp`, `BiQuad.cpp` | Upstream source; core/UI tests | Low/high-pass implementations reused. |
| Biquad `BP` | PASS | Same | Same | Band-pass implementation reused. |
| Biquad `LS`/`LSC`, `HS`/`HSC` | PASS | Same | Same | Shelf aliases reused. |
| Biquad `NO`, `AP` | PASS | Same | Same | Notch/all-pass reused. |
| `Filter N: OFF None` / inactive-filter syntax | PASS | `BiQuadFilterFactory::createFilter` returns no filter for non-`ON` states and `None`; `FilterEngine::loadConfiguration` ignores empty factory results; official `Setup/config/example.txt` and `demo.txt` contain `OFF None`/`ON None` | `Engine::parseConfigFile` handles explicit `OFF` slots and `ON None` as no-ops; `tests/core_tests.cpp` verifies zero active filters and unchanged samples for `OFF None`, `OFF PK ...` and `ON None`; `skyapo-core` PASS | Valid upstream inactive slots no longer fail strict Linux config validation; malformed active filter parameters remain validated as errors. |
| `Filter: ON IIR Order ... Coefficients ...` | PASS | `IIRFilterFactory.cpp`, `IIRFilter.cpp` | Upstream source; `tests/core_tests.cpp`, `IIRFilterEditor`, renderer/config tests | Order/coefficient validation and core processing are present. UI rejects unsupported raw syntax rather than silently rewriting it. |
| `GraphicEQ` frequency/gain pairs | PARTIAL | `GraphicEQFilterFactory.cpp`, `GraphicEQFilter.cpp` | Upstream filter; FFTW3f-gated registration in `Engine`; GUI reuses upstream GraphicEQ; response/offline tests | Requires FFTW3f; implementation uses fixed `16384` filter parameter like upstream. No claim for every rate/block/device combination or host-level failure/recovery. |
| `Convolution` impulse response | PARTIAL | `ConvolutionFilter.cpp`, `ConvolutionFilterFactory.cpp`, `libHybridConv-0.1.1/` | `src/platform/linux/ConvolutionFilterLinux.cpp`; FFTW3f/libsndfile; `skyapo-core`, offline and realtime E2E | Valid-IR DSP semantics, same-rate requirement, modulo channel mapping and the fixed block after initial setup are shared with upstream. Linux differs in path/file handling and rejects the candidate config on invalid/mismatched IR, where upstream logs and can leave a pass-through filter; that error-policy delta is why behavior is PARTIAL. Neither provides automatic resampling or arbitrary matrix convolution. |
| `Delay` in milliseconds | PASS | `DelayFilterFactory.cpp`, `DelayFilter.cpp` | Upstream filter; `tests/core_tests.cpp`, PDC E2E | Converted using negotiated sample rate. |
| `Delay` in samples | PASS | Same | Same | Explicit sample units retained. |
| `Channel` selection and restore behavior | PARTIAL | `ChannelFilterFactory.cpp`, `ChannelFilter.cpp` | Upstream filter, Linux `ChannelHelper`; core and PipeWire mono/stereo tests | Only known mapped SPA layouts/names are safe; `RC` and various less common EAPO/SPA positions are not comprehensively mapped. Runtime graph caps channels at its own maximum. |
| `Copy` direct/fan-in channel routing and gains | PASS | `CopyFilterFactory.cpp`, `CopyFilter.cpp` | Upstream filter plus `PdcCopyFilter` in `Engine.cpp`; core and realtime tests | Main channel assignment/sum/factor/dB behavior is present. PDC adapter preserves route timing for supported channel maps. |
| `Device` text pattern command | PARTIAL | `DeviceFilterFactory::matchDevice` | `DevicePatternMatcher.cpp`, Engine's selected `deviceMatchText`; core and E2E device-filter tests | Pattern groups/substring matching are ported against the selected PipeWire source identity. It is not EAPO endpoint selection across render and capture devices; GUID/device metadata context differs. |
| `Stage: capture` | PASS | `StageFilterFactory.cpp` | Parser stage gate in `Engine::parseConfigFile`; core/E2E | Capture is the active runtime stage. |
| `Stage: pre-mix` execution | MISSING | `StageFilterFactory::createFilter`, `FilterEngine` pre-mix handling | `Engine::parseConfigFile` accepts the token, but `stageActive` is false outside capture; no pre-mix graph exists | Syntax acceptance is not processing: this upstream placement has no Linux runtime implementation. |
| `Stage: post-mix` execution | MISSING | `StageFilterFactory::createFilter`, `FilterEngine` post-mix handling | `Engine::parseConfigFile` accepts the token, but `stageActive` is false outside capture; no post-mix graph exists | Syntax acceptance is not processing: this distinct upstream placement has no Linux runtime implementation. |
| Stage editor pre-mix/post-mix selection | MISSING | `Editor/guis/StageFilterGUI*` | Current `MainWindow::rebuildRows()` disables `preMixCheckBox` and `postMixCheckBox`; `docs/UI.md`, `tests/ui_tests.cpp` | The controls are visible but intentionally disabled because Linux has no playback-stage graph. Existing text is preserved; the user cannot select these placements through the visual editor. At pinned `HEAD` before the dirty UI change, the enabled widget could serialize them (then UI_ONLY); this row reflects current working-tree behavior. |
| `Include` relative paths and nested includes | PASS | `IncludeFilterFactory.cpp`, `FilterEngine` file iteration | `ConfigSource`, recursive `Engine::parseConfigFile`; `config_source`, `core`, watcher tests | Linux uses `std::filesystem`, canonical paths, explicit cycle/depth diagnostics; supports quoted paths and `#` in quoted path. Windows `MAX_PATH` behavior intentionally not copied. |
| Include-scoped Device/Stage semantics | PASS | `DeviceFilterFactory::endOfFile` restores matching to the outer file's eligible default; `StageFilterFactory::startOfFile/endOfFile` push and restore the outer stage | `Engine::parseConfigFile` only enters an Include while active, gives recursion a local device match state, and copies/restores `stageActive`; `tests/core_tests.cpp` covers one-level and nested Include-local controls with numerical gain checks | Source behavior matches the upstream per-file boundary. The nested regression is Linux-side rather than exhaustive differential execution of upstream and Linux parsers. |
| `If` / `ElseIf` / `Else` / `EndIf` | PASS | `IfFilterFactory.cpp` | Engine condition stack; `tests/core_tests.cpp`, config tests | Nested branches, unmatched directives and errors have explicit code/tests. Inactive branches are skipped. |
| `Eval` and backtick inline expressions | PARTIAL | `ExpressionFilterFactory.cpp` | `Engine::evaluateExpression`/`expandInlineExpressions`; core tests | Implemented and tested on the normal MuParserX path; the muParser fallback explicitly rejects `Eval` and backticks. No Registry lookup. EAPO historically logs some expression errors while Linux rejects the candidate configuration transactionally. |
| EAPO expression operators, string `+`, prefix `not`, regex functions | PARTIAL | `ExpressionFilterFactory`, `parser/LogicalOperators`, `StringOperators`, `RegexFunctions` | MuParserX configuration in `Engine.cpp`; copied/adapted logical/string operators; core tests | Main operators and regexSearch/regexReplace exist on the MuParserX build. The optional muParser fallback does not register the EAPO regex functions and has reduced syntax; exhaustive equivalence is unproven. |
| EAPO Registry function `readRegString` | N/A_WINDOWS | `parser/RegistryFunctions.cpp`, registered by `ExpressionFilterFactory::initialize` | No Linux Registry adapter or matching function | Directly queries the Windows Registry; no portable Linux equivalent or contract exists. Existing configs using it need migration and will be rejected. |
| EAPO Registry function `readRegDWORD` | N/A_WINDOWS | `parser/RegistryFunctions.cpp`, registered by `ExpressionFilterFactory::initialize` | No Linux Registry adapter or matching function | Directly queries the Windows Registry; no portable Linux equivalent or contract exists. Existing configs using it need migration and will be rejected. |
| EAPO expression constants `sampleRate`, `inputChannelCount` | PASS | `ExpressionFilterFactory::initialize` | `Engine::loadConfig`; core tests | Values are defined from the negotiated capture format. |
| EAPO expression constant `outputChannelCount` | PARTIAL | `ExpressionFilterFactory::initialize`, `FilterEngine::getOutputChannelCount` | `Engine::loadConfig` defines it as `channelCount` | Exists, but Linux currently aliases output count to input count; no independent output topology is represented. |
| EAPO expression constant `deviceName` | PARTIAL | `DeviceFilterFactory::initialize`, `FilterEngine::getDeviceName` | MuParserX `Engine` expression context defines it from PipeWire `node.description`; `tests/core_tests.cpp` and `skyapo-pipewire-e2e-device-filter` | Available on the normal MuParserX path. The classic muParser fallback cannot represent EAPO string constants and rejects string expressions. |
| EAPO expression constant `connectionName` | PARTIAL | `DeviceFilterFactory::initialize`, `FilterEngine::getConnectionName` | MuParserX `Engine` expression context maps it to PipeWire stable `node.name`; core and device-filter E2E tests | Best-effort Linux mapping, not a hardware connector name; classic muParser fallback lacks string expressions. |
| EAPO expression constant `deviceGuid` | N/A_WINDOWS | `DeviceFilterFactory::initialize`, `FilterEngine::getDeviceGuid` | No equivalent constant in Linux expression parser | Windows endpoint GUID is not a portable identity. Linux has stable PipeWire identity, but no GUID-compatible contract; configs using this variable need migration. |
| EAPO expression constant `stage` | PARTIAL | `StageFilterFactory::initialize` | MuParserX `Engine` expression context defines the active Linux stage as `capture`; core and device-filter E2E tests | This capture runtime has a single stage; classic muParser fallback cannot represent the string constant, and Windows render-stage values do not exist here. |
| Comments, quoted `#`, command whitespace | PARTIAL | EAPO parser/factories and sample configs | `stripInlineComment`, trim/line reader; `config_source` tests | Basic comments and quoted hashes are supported. Full lexical compatibility including all escaping/locale edge cases has not been differential-tested. Commands are effectively case-sensitive as in factories; this is a potential user surprise. |
| Numeric decimal comma/locales and command case handling | PARTIAL | Preamp/Delay/BiQuad/GraphicEQ factories have different comma handling | Same upstream factories plus Engine's command parser | Common decimal-comma handling survives in reused factories; locale-specific formatting and command case are not one uniform grammar. Filter subtype aliases are counted only in the subtype atoms, not again here. |
| Strict config errors with file/line and failed-reload rollback | PASS | Upstream log/parser handling | `Engine::ConfigError`, JSON check, daemon reload transaction; config-check/watcher/E2E tests | Linux deliberately makes unknown directives/errors fatal and retains last valid graph; differs from upstream's sometimes logged/ignored factory errors. |
| Unknown command/raw line preservation in GUI | PARTIAL | EAPO FilterTable raw text view/edit | `ConfigFile`, Linux row fallback and round-trip UI tests | Preserved and editable as raw text; no semantic editor or guaranteed validation until Save & Check/Reload. |

### Plugins

| Upstream capability | Status | Upstream evidence | LinuxAPO evidence / tests | Difference or caveat |
|---|---|---|---|---|
| Original EAPO VST2 Windows ABI host | N/A_WINDOWS | `VSTPluginFilter*`, `VSTPluginLibrary/Instance`, Windows ABI headers | Separate format-neutral `IPluginHost/IPluginInstance` and Linux backends | Windows DLL loading, HWND editor embedding and Windows path behavior are intentionally not ported. |
| Native LV2 backend | PARTIAL | No equivalent native LV2 host in EAPO | `LV2PluginHost.cpp`; Engine registration; LV2/core/realtime fixtures | LinuxAPO addition. Host/runtime are wired; it accepts a constrained LV2 feature set and matching audio-port topology. Config-directive parsing is scored separately. This status is not downgraded for absent UI or test breadth. |
| Native CLAP backend | PARTIAL | EAPO VST-only plugin factory | `CLAPPluginHost.cpp`; Engine registration; core/realtime/latency/state/bypass fixtures | LinuxAPO addition. Host/runtime are wired for a constrained main-bus/extension topology. Host bypass syncs advertised `CLAP_PARAM_IS_BYPASS`, otherwise dry bypass is used. Config-directive parsing and UI limitations are scored separately. |
| Native VST3 backend | PARTIAL | EAPO has no native VST3 host | `VST3PluginHost.cpp`; Engine registration; fixture/realtime/parameter/failure/latency/native-bypass tests | LinuxAPO addition. Host/runtime are wired, but audio processing is mono/stereo only and the supported host surface is restricted. Config-directive parsing and UI limitations are scored separately. |
| Optional FST-backed VST2 (`Plugin: VST2 ...`) | BACKEND_ONLY | EAPO offers VST2 syntax but Windows module implementation | `VST2PluginHost.cpp`, `VST2PluginFilter.cpp`; opt-in build and prototype tests | Linux `.so` ABI via FST; opt-in/restricted, not Windows DLL support, distro packages disable it; no editor UI. |
| Legacy `VSTPlugin:` translation | BACKEND_ONLY | `VSTPluginFilterFactory` supports `Library`, parameters, `ChunkData` | `VST2PluginFilter.cpp` parses restricted compatibility subset | Only opt-in FST/VST2 and Linux-compatible modules/parameters; Windows binaries and legacy inline `ChunkData` unsupported; no UI. |
| Plugin discovery/catalog | PARTIAL | Windows VST GUI browses EAPO plugin directory | CLI `plugin list/scan/info`; UI Tools → Browse installed plugins and Add Filter → Installed plugin | LV2/CLAP/VST3 discovery feeds a catalog and insertion picker; optional VST2 has no discovery/list/insertion UI. The picker inserts a bare directive; parameter values are edited separately through Tools, not in the picker. |
| Live plugin parameter readback/control | BACKEND_ONLY | EAPO VST GUI edits parameter widgets | `skyapo plugin info/set`, IPC live control; plugin-specific tests; saved-config editor in `MainWindow.cpp` | Live-value readback/control is CLI/API only; the separate UI config editor edits saved LV2/CLAP/VST3 values but does not query/control active values. Symbol/ID conventions vary by format. |
| Plugin state persistence in runtime | PASS | `VSTPluginFilter::getChunkData`, `getParamMap` and configuration serialization | `IPluginStatePersistence`; LV2/CLAP/VST3/VST2 sidecars; Engine lifecycle save/restore; `tests/lv2_live_control_test.cpp` | All four hosts implement state persistence where the plugin exposes the format API, and all four have fixture round-trip test coverage. CLAP/VST2/VST3 use their focused tests; LV2 saves, creates a new Engine and verifies restored output. Controlled fixtures do not prove arbitrary third-party plugin state compatibility. Sidecars are not editor-visible. |
| Plugin latency/PDC in runtime | PASS | EAPO VST processing path reports latency to the engine | Engine PDC, plugin latency interfaces and LV2/CLAP/VST3/VST2 fixtures | Runtime compensates reported plugin latency and refreshes/rebuilds downstream compensation. Reported plugin latency is not end-to-end latency. Missing editor display does not downgrade runtime behavior. |
| Native plugin instance insertion in editor | PASS | `VSTPluginFilterGUIFactory` exposes VST instance configuration | `MainWindow.cpp::populateAddPopupMenu`, installed LV2/CLAP/VST3 picker; `tests/ui_tests.cpp` | UI inserts directives for all native formats currently exposed by LinuxAPO. FST/VST2 has no picker/insertion flow and is a separate MISSING row. This row scores insertion only, not parameter/state configuration or native GUI embedding. |
| FST/VST2 instance insertion in editor | MISSING | EAPO VST GUI can choose its Windows VST library | No VST2 scanner/catalog or Add Filter entry; manual `Plugin: VST2` config only | Optional backend is not exposed through the editor. This is distinct from native-format insertion. |
| Plugin parameter editor UI | PARTIAL | `VSTPluginFilterGUI*`, `VSTPluginFilterGUIDialog*` parameter widgets | `MainWindow::configureSelectedPluginParameters`, CLI `plugin info`, `tests/ui_tests.cpp` | Tools editor writes writable LV2/CLAP/VST3 config values and preserves comments/line endings. It does not provide active-value readback, VST2 parameters, plugin state editing or native GUI hosting; Save & Reload is needed to apply edits. |
| Native plugin GUI hosting | MISSING | EAPO embeds the VST editor window (`VSTPluginFilterGUIDialog`) | No Linux plugin GUI embedding or host UI bridge | Windows `HWND` mechanics are N/A_WINDOWS, but absence of any equivalent native Linux plugin UI remains a user-facing gap. yabridge UI is not hosted/managed. |

### PipeWire/runtime and device management

| Upstream capability | Status | Upstream evidence | LinuxAPO evidence / tests | Difference or caveat |
|---|---|---|---|---|
| Windows capture APO processing | REPLACED | `EqualizerAPO/` COM APO classes and `FilterEngine` capture mode | `Runtime` PipeWire capture filter → Engine → virtual source; mono/stereo E2E and hardware evidence in `docs/REALTIME.md` | Valid Linux capture replacement, not an APO insert into every application/endpoint. |
| Playback/sink audio processing capability | MISSING | EAPO render endpoint processing path (`EqualizerAPO/`, `FilterEngine`) | No Linux PipeWire playback sink/filter output path | The Windows COM/APO insertion mechanism is Windows-specific, but the user-visible playback-processing capability itself has no Linux implementation. Current LinuxAPO is capture/microphone only. |
| Physical capture enumeration/selection | PARTIAL | `DeviceAPOInfo`, Windows endpoint enumeration | `DeviceManager`, CLI `device list/set/current`, GUI combo; device identity/E2E tests | Linux enumerates/selects one capture source for the daemon; EAPO's selector covers endpoint installation/configuration across playback and capture endpoints. Stable PipeWire properties replace numeric runtime IDs but not the broader selector behavior. |
| Device Selector install/enabling and APO registration | N/A_WINDOWS | `DeviceSelector/*`, Setup registry/APO registration | No registry analogue | Linux users select capture node and start a user daemon; Windows LFX/GFX/SFX/EFX toggles do not apply. |
| Input/output device/default distinction and metadata | PARTIAL | `DeviceAPOInfo` captures playback/capture/default/state/device topology | PipeWire DeviceManager lists capture sources with description/channels/rate/stable identity; separate default render volume monitor | No equivalent full Device Selector tree, device install state, endpoint connector taxonomy or playback processor selection. |
| Selected-device switching while daemon runs | PASS | EAPO device selection happens through Windows host lifecycle | Runtime setting monitor and graph reconfiguration; `skyapo-pipewire-e2e-device-switch` | Tested private graph; behavior depends on PipeWire graph/source availability. |
| Capture-source hotplug/disconnect/reappearance | PASS | Windows engine/device host reconnect semantics | PipeWire registry removal/reappearance handling; `skyapo-pipewire-e2e-source-replug` | Implementation and synthetic graph recovery exist. The E2E uses a deterministic PipeWire source, not a physical microphone/vendor matrix; that is a confidence/test-coverage limitation, not an implementation gap. |
| PipeWire server restart/reconnect | PASS | Not applicable to EAPO's Windows host | `Runtime` restart/reconnect; `skyapo-pipewire-e2e-server-restart` | Tested with private PipeWire graph, not every desktop-session manager behavior. |
| PipeWire format negotiation / sample-rate change | PASS | EAPO `FilterEngine::initialize` receives format from APO host | Runtime negotiates F32 planar and rebuilds Engine on rate/quantum change; `...renegotiate`, transition-format tests | Format currently centered on float32; unsupported formats/layouts fail or trigger rebuild, not arbitrary format conversion. |
| Quantum/block-size changes | PASS | EAPO initialize/process is host-block driven | Runtime format event and Engine rebuild; `...renegotiate`, fixed convolution quantum tests | Convolution has a fixed-block restriction; non-convolution processing slices within bounded work buffer. |
| Channel mapping and multichannel layouts | PARTIAL | `ChannelHelper`/speaker mask and EAPO channel-name conventions | `PlatformChannels.h`, Runtime channel mapping, MaxChannels; deterministic mono/stereo E2E | Common mono/stereo and common 5.1/7.1 labels mapped in code; uncommon positions/masks and real multichannel microphone paths lack complete coverage. |
| Virtual microphone/source | REPLACED | Windows APO output stays within Windows audio graph | Native PipeWire virtual source `SkyAPO Virtual Mic`; native consumer E2E | Application sees PipeWire source; it is not a system-installed hardware endpoint and depends on running daemon. |
| Config file watching / hot reload | REPLACED | `FindFirstChangeNotification`, Registry watch and background notification thread in `FilterEngine` | `ConfigWatcher` with inotify, off-thread validation/build, graph swap; watcher and PipeWire reload E2E | Linux implementation tracks Include dependencies and rollback; timing/event semantics differ from Win32. |
| Bad configuration rollback | PASS | Upstream loader replaces current config through internal transitions but errors are logged | Transactional candidate Engine and last-valid graph; config/watcher/E2E tests | Stronger explicit failure behavior; parser features missing still reject configs. |
| Transition between valid graphs | PASS | `FilterEngine::process` current/next configurations/cosine transition | `Engine::processTransitionTo`, Runtime crossfade; core/E2E transition tests | Linux graph transition and bounded callback handoff are separate code. |
| Realtime callback allocation/filesystem/config parsing | PASS | Upstream `IFilter::process` AVRT guidance and prebuilt filters | `RealtimeAudit`, preallocation, runtime tests assert callback allocation/deallocation zero | Auditor covers wrapped/executable allocations and explicitly excludes allocations internal to arbitrary shared libraries; do not interpret as proof for all plugin code. |
| Xrun/overrun and process timing diagnostics | PARTIAL | EAPO `LogHelper`/Benchmark CPU estimate | Runtime tracks callback average/max/overruns and RMS | No full host xrun telemetry integration or end-to-end latency measurement; daemon does not expose all historical EAPO logs. |
| Daemon lifecycle/control/status | REPLACED | Windows service/APO DLL lifecycle and installer | `skyapod`, CLI IPC, systemd user template; daemon/IPC/restart E2E | Per-user Linux process/service; no Windows service/COM registration semantics. |
| Crash supervision/restart after process death | PASS | Windows audio host/service restart behavior | `packaging/skyapod.service.in` configures `Restart=on-failure`; optional `skyapo-pipewire-e2e-systemd-crash-restart` kills the daemon with `SIGKILL` under a transient user unit and verifies a new PID, increased `NRestarts`, restored streaming/links, and independent -6 dB capture | Verified for the supplied systemd user unit when enabled. It does not supervise a manually launched daemon, isolate plugin crashes, or guarantee restart when users disable/omit the unit. |
| Clean shutdown | PASS | COM/DLL teardown, FilterEngine thread join | Runtime callbacks drained, PipeWire objects destroyed, daemon control lifecycle tests | Subject to external PipeWire/client teardown race coverage. |

### Editor and response/diagnostic tools

| Upstream capability | Status | Upstream evidence | LinuxAPO evidence / tests | Difference or caveat |
|---|---|---|---|---|
| Open/save configuration | PARTIAL | `Editor/MainWindow.cpp`, File menu, tabs | Linux Open/Save/Save As, recent files, `ConfigFile`; UI tests | Single active document, no multi-tab/new unsaved config workflow. Save is explicit rather than EAPO Instant mode. |
| Save As, relative Include rebasing | PASS | EAPO Save As/config path logic | `MainWindow::saveConfigAsPath`, `rebaseRelativeIncludes`; UI tests | Linux explicitly refuses unsafe expression-based Include rebasing instead of silently changing meaning. |
| Add Filter categories/templates | PARTIAL | `Editor/IFilterGUIFactory` and all GUI factories | Linux Add Filter: Preamp, Include, Channel, Copy, biquads, IIR, Delay, Stage, Device match, Convolution, GraphicEQ, and discovered LV2/CLAP/VST3 insertion picker; UI test asserts Include-template presence | Missing LoudnessCorrection/Expression/Comment and FST/VST2 insertion templates. The Include template adds a starter `child.txt` path; file selection/editing is through the existing Include row. |
| Per-filter widgets | PARTIAL | All `Editor/guis/*` | Reused Preamp/BiQuad/Delay/Stage/GraphicEQ; Linux Convolution/Include/ChannelCopy/IIR and `DeviceFilterEditor` adapters | The Device widget edits a match pattern but does not reproduce EAPO's endpoint/device-state dialog. No Loudness Correction, Expression, native plugin GUI, or complete upstream filter widget set. Qt UI test covers creation/edit/save serialization, not full device-selector behavior. |
| Filter enable/disable via controls | PARTIAL | Upstream filter GUIs serialize on/off where applicable | Reused selected upstream widgets and raw line editor | No uniform enable switch for every directive; disabled semantics depend on each command. |
| Remove/add/reorder filters | PARTIAL | Row Add/Remove/edit; FilterTable drag/drop/copy/paste/multiselect | Linux rows add/remove/multiselect-delete; Alt+Up/Down move; tests | No upstream drag/drop, clipboard filter block copy/paste, or full FilterTable workflow. |
| Raw text / unknown command preservation | PARTIAL | Upstream row offers text editing | Linux fallback row preserves raw line and allows editing; `ConfigFile` byte-preserving tests | Complex comment/conditional blocks have no AST awareness; visual edit/reorder can alter scope/order. |
| Device combo and device command editor | PARTIAL | Main Editor device combo and `DeviceFilterGUI` with dialog/device-state data | Linux input-only PipeWire combo, refresh/select; CLI device commands; `src/ui/DeviceFilterEditor.cpp`, Add Filter `Device match` template in `src/ui/MainWindow.cpp`; `tests/ui_tests.cpp` | A basic editor serializes the `Device:` match pattern, but does not enumerate playback config targets or reproduce the richer EAPO endpoint/device-state dialog. Qt test verifies row creation and edit/save round-trip, not endpoint-selector parity. |
| Channel layout selection/editor | PARTIAL | MainWindow channel configuration combo plus channel/copy topology dialogs | Linux chooses actual PipeWire format and has Channel/Copy widgets | No independent arbitrary channel mask editor; selector reflects currently enumerated source only. |
| Include navigation/editor | PARTIAL | Include GUI file chooser | Linux IncludeEditor/navigation and Save As rebasing; UI tests | No tree of all includes or upstream's full file-tab workflow. |
| GraphicEQ interactive graph/editor | PASS | `GraphicEQFilterGUI*`, plot scene | Upstream GUI files compiled; test/resource audit | Requires FFTW3f; exact UI/resource set selectively adapted. |
| Convolution chooser/widget | PARTIAL | `ConvolutionFilterGUI*` and impulse file dialog | Linux `ConvolutionEditor` | Path chooser/editing works; advanced upstream convolution options are limited because config itself is single IR path. |
| Frequency response magnitude analysis | PARTIAL | `AnalysisThread`, `AnalysisPlotScene/View`, analysis dock | `ResponseAnalysis::analyzeConfigResponse`, Analyze Response button; UI numerical test | Offline theoretical Engine impulse response only; plugin directives are disabled/rejected for analysis; not a measured live-path plot. |
| Analysis source selection (`config.txt` vs current file) | PASS | `Editor/MainWindow.ui::startFromComboBox`, `MainWindow::startAnalysis` | `src/ui/MainWindow.cpp::analyzeResponse`, `analysisSource` combo; `tests/ui_tests.cpp` numerically verifies saved default `config.txt` yields −6 dB and unsaved current document yields −3 dB | Both upstream source choices are exposed and actually select the corresponding analysis snapshot. Relative Include paths resolve beside the selected source. |
| Analysis FFT resolution | PARTIAL | `Editor/MainWindow.ui::resolutionSpinBox`, `MainWindow::startAnalysis` | `src/ui/MainWindow.cpp::analysisResolution`, `analyzeResponse`; `ResponseAnalysis`; `tests/ui_tests.cpp` verifies 8192 samples produce 4096 bins | User-selectable, persisted sizes from 1024 to 131072 samples work. This is narrower than upstream's 128–8388608 range; Linux processing uses 1024-frame blocks and therefore only exposes supported multiples. |
| Plugin-inclusive response analysis | MISSING | `AnalysisThread` initializes/processes the selected EAPO configuration, including its VST filter when usable | `ResponseAnalysis` disables plugin loading; no plugin response curve | This is separate from plugin-free magnitude analysis. Lack of a plugin-response test is not the reason; the analyzer explicitly omits plugin processing. |
| EAPO response-analysis latency estimate | MISSING | `Editor/AnalysisThread`, `MainWindow::updateAnalysisPanel` | `ResponseAnalysis` computes response magnitude only; runtime exposes plugin-reported latency, not this analysis result | No EAPO-style estimated filter-chain latency in the analysis panel. Runtime plugin latency and PipeWire latency are different quantities. |
| EAPO response-analysis processing CPU estimate | MISSING | `Editor/AnalysisThread`, `MainWindow::updateAnalysisPanel` | Runtime has callback timing, but no equivalent analysis CPU percentage result | Live callback timing is not the upstream analysis-thread CPU estimate. |
| EAPO response-analysis initialization time | MISSING | `Editor/AnalysisThread`, `MainWindow::updateAnalysisPanel` | No equivalent analysis result in Linux response-analysis UI | Engine build/init occurs but is not measured and presented as this analysis field. |
| EAPO response-analysis peak gain | MISSING | `Editor/AnalysisThread`, `MainWindow::updateAnalysisPanel` | No equivalent peak-gain result in Linux response-analysis UI | Runtime RMS/ratio is not a peak-gain estimate. |
| Runtime troubleshooting/status | PARTIAL | EAPO install warnings and Device Selector test dialog | Linux daemon status includes input/output RMS, active links, filter chain, timing, overruns, callback allocation audit and plugin errors | More runtime metrics than EAPO UI in some areas, but large text label/no graph and lacks host-level Windows install diagnosis. |
| File/Edit/View/Settings menus and shortcuts | PARTIAL | `MainWindow.ui`: New/Open/Save/Save As/Exit, Cut/Copy/Paste/Delete/Select All, Toolbar, Analysis panel | Linux File Save As/Open Recent, Tools plugin catalog, Help/About, toolbar buttons, Delete and Alt+Up/Down | Linux omits New/multiple tabs, general Edit/View/Settings menus and EAPO shortcuts. |
| Preferences, language selection, localization | PARTIAL | QSettings prefs, Language menu, translations under Editor/DeviceSelector/UpdateChecker | Linux persists window geometry/state/recent files, Qt `tr()` calls | No translation catalogs/language UI or EAPO global/file-specific preferences parity. |
| Dark mode/theme integration | PASS | `GUIHelper::isDarkMode`, `CustomStyle` | Reused upstream GUI helper/style where compiled | Qt/system style support is present; an older Qt CI environment was observed to fail compilation on `QStyleHints::colorScheme`, a build portability issue, not parity. |

### Device Selector/Configurator, installer, benchmark, diagnostics

| Upstream capability | Status | Upstream evidence | LinuxAPO evidence / tests | Difference or caveat |
|---|---|---|---|---|
| Stable device persistence | PARTIAL | EAPO persists Windows device string/identity via preferences/Registry | `Settings::select()` persists `device.serial` or `device.bus-id` when advertised; `findSelectedDevice()` resolves on that value and falls back to `node.name` for older/identity-less selections; `skyapo-pipewire-e2e-source-replug-stable-id` verifies a renamed synthetic source reconnects by serial. | Source and fixture recovery are tested; actual hardware/vendor coverage remains unverified, and devices without serial/bus-id use the less stable `node.name` fallback. Do not confuse limited test coverage with missing implementation. |
| Device functional test/troubleshooter | PARTIAL | `DeviceTestDialog/Thread` probes each installed APO mode | `pipewire_e2e_test.py`, `skyapo status --verbose`, CLI/UI controls | Automated private-graph tests are strong but no interactive microphone loopback/test-record dialog or diagnostic wizard. |
| Benchmark | PARTIAL | `Benchmark/Benchmark.cpp` measures upstream filter processing | `skyapo-bench`, `tests/benchmark_test.py` using real Preamp chain | A Linux benchmark exists, but setup and metrics differ; it is not a full port of all upstream benchmark modes. |
| Logging | PARTIAL | `LogHelper`, trace/debug files, ETW/Win32-oriented paths | daemon stderr/journald/status diagnostics | No complete EAPO log destination/filter verbosity/configuration parity. |
| Installer/package/service integration | REPLACED | NSIS architectures, DLL/registry/device installation, update checker | CMake install, Arch/DEB/RPM/tar/Nix sources, user service | Linux formats exist; no Windows installer/update checker semantics. Distribution clean-install and every package-manager lifecycle are not proven by source parity. |
| Update checker | N/A_WINDOWS | `UpdateChecker/*` Windows UI/network implementation | No updater | No auto-update feature; Linux package managers/releases are the route. |

## Configuration-language parity, atomized

The upstream filter factory registration is authoritative: `FilterEngine.cpp` registers Device, If, Expression, Include, Stage, Channel, IIR, BiQuad, Preamp, Delay, Copy, Convolution, GraphicEQ, VSTPlugin and LoudnessCorrection. `ExpressionFilterFactory` additionally consumes `Eval`; conditional factory handles `ElseIf`, `Else`, `EndIf`. This yields 17 upstream EAPO command spellings: `Preamp`, `Filter`, `GraphicEQ`, `Convolution`, `Delay`, `Channel`, `Copy`, `Device`, `Stage`, `Include`, `If`, `ElseIf`, `Else`, `EndIf`, `Eval`, `VSTPlugin`, `LoudnessCorrection`. LinuxAPO adds `Plugin:` as an eighteenth, non-EAPO spelling. `Filter` has two filter families and is expanded into eight biquad types plus IIR in the score below.

Config score atoms and points (PASS = 1, PARTIAL = 0.5, BACKEND_ONLY = 0.75, UI_ONLY = 0.25; BROKEN/MISSING = 0; REPLACED = 1 only for equivalent runtime behavior; N/A excluded):

| Atom group | Atoms | Earned points | Evidence summary |
|---|---:|---:|---|
| Preamp | 1 | 1 | Upstream gain factory/math and numeric test. |
| Biquad subtypes: PK/PEQ/Modal, LP/LPQ, HP/HPQ, BP, LS/LSC, HS/HSC, NO, AP | 8 | 8 | Names/aliases in upstream factory; Linux calls that factory. |
| IIR | 1 | 1 | Upstream factory and editor/engine tests. |
| GraphicEQ | 1 | 0.5 | Syntax/factory and DSP exist when optional FFTW3f support is built; not available in builds without that dependency. |
| Convolution | 1 | 0.5 | Valid-IR command/DSP behavior and fixed-block lifecycle are shared; partial credit is for Linux's different invalid/mismatched-IR config failure behavior, not for a test gap. |
| Delay units: ms, samples | 2 | 2 | Upstream factory and sample-rate conversion. |
| Channel selection semantics and numeric channel selectors | 2 | 1 | Common labels work; channel identity/layout coverage is partial. |
| Copy assignment/routing/gain syntax | 1 | 1 | Upstream factory/filter plus PDC route wrapper. |
| Device pattern groups | 1 | 0.5 | Matching ported, endpoint metadata/scope differs. |
| Stage placement: capture | 1 | 1 | Linux capture graph executes this placement. |
| Stage placement: pre-mix | 1 | 0 | Parser accepts the spelling, but there is no Linux pre-mix graph. |
| Stage placement: post-mix | 1 | 0 | Parser accepts the spelling, but there is no Linux post-mix graph. |
| Include path resolution, quoting, nesting, cycle/depth errors | 1 | 1 | Linux implementation and source/watcher tests. |
| If/ElseIf/Else/EndIf | 4 | 4 | Explicit nested control stack and validation. |
| Eval and inline backtick expansion | 2 | 1 | Both work on MuParserX; the classic muParser fallback explicitly rejects them. |
| Regex search/replace | 2 | 1 | Both EAPO functions are registered on the MuParserX path; the optional muParser fallback registers neither. Partial credit reflects build-dependent syntax availability, not missing tests. |
| Runtime variables: sampleRate/inputChannelCount/outputChannelCount | 3 | 2.5 | First two are equivalent; output count exists but aliases capture count instead of supporting independent input/output topology. |
| Runtime variables: deviceName/connectionName/stage | 3 | 1.5 | MuParserX maps them to node.description, best-effort node.name, and capture; string constants are unavailable in the classic muParser fallback. |
| Runtime variable: deviceGuid | 1 | excluded | Windows endpoint GUID has no portable Linux contract; see N/A_WINDOWS rows. |
| Registry read functions | 2 | excluded | Direct Windows Registry APIs have no portable Linux contract; see N/A_WINDOWS rows. |
| Comments/quoted hash and basic whitespace | 2 | 1.5 | Common behavior; edge lexical cases lack differential tests. |
| Case/locale/number formatting | 2 | 1 | Factory-dependent case/comma behavior; not complete locale parity. |
| LoudnessCorrection config directive | 1 | 0.5 | Upstream filter registered and Linux provider exists, but depends on default render endpoint uniform volume and is unavailable in many setups. |
| EAPO `VSTPlugin:` legacy grammar | 1 | 0.5 | Restricted, opt-in FST/VST2 compatibility only. |
| Native Linux `Plugin:` directive | 1 | 1 | Added Linux syntax; not an EAPO compatibility point, shown for inventory only. |
| `Filter N: OFF None` disabled slots | 1 | 1 | `OFF` and `ON None` no-op forms now match upstream behavior and have sample-preservation tests. |
| **EAPO applicable denominator** | **43** | **32** | Excludes Linux-only `Plugin:` and three Windows-only atoms (`readRegString`, `readRegDWORD`, `deviceGuid`). The listed applicable atom groups sum to 43. |

**CONFIG PARITY = 32 / 43 = 74.4%.** The added Linux `Plugin:` row is not included in either total. This score counts a filter type as one atom, not every numeric grammar production. It is a coverage index, not a claim of 74.4% legacy config acceptance. A build without FFTW3f or optional plugin hosts has lower feature availability; the fallback muParser build also loses some EAPO expression behavior. Windows-only Registry/GUID rows remain documented as migration incompatibilities but are excluded from the portable-parity denominator.

Syntax details found in source: factories generally require exact command case; common command/parameter whitespace is trimmed; comments begin at `#` outside quotes in Linux's line scanner; Include paths may be quoted and relative to the including file; nested includes have cycle/depth guards. BiQuad recognizes PK/PEQ/Modal, LP/LPQ, HP/HPQ, BP, LS/LSC, HS/HSC, NO, AP; Filter is ON-based and supports Q/BW/slope according to type. Delay accepts `ms` and `samples`. Copy supports source sums, direct assignments, scalar/dB forms and constants in the upstream factory. Device uses OR-separated semicolon groups and AND words (case-insensitive substring matching, stripping GUID-like tokens for name matches). Stage parsing is a Linux simplification. Sample-rate-dependent delay/filter setup is delegated to actual upstream filter initialization. `readRegString`/`readRegDWORD` have no Linux equivalent.

## DSP parity scorecard

Atomic denominator = 21 applicable DSP capabilities. PASS = 1, PARTIAL = 0.5, BACKEND_ONLY = 0.75, UI_ONLY = 0.25, other implementation statuses = 0; Windows-only stage processing excluded here because capture DSP itself is measured separately.

| Capability | Points | Evidence |
|---|---:|---|
| Preamp | 1/1 | `tests/core_tests.cpp`: samplewise −6 dB multiplication. |
| Eight biquad variants | 8/8 | Upstream `BiQuad` and `BiQuadFilter` compile/use; representative renderer/UI/response tests. Individual response assertions do not exhaust all 8 transfer functions. |
| IIR | 1/1 | Upstream `IIRFilter` runs; order/coefficient tests. |
| Delay | 1/1 | Upstream delay, PDC tests and delay/PDC PipeWire E2E. |
| Channel selection | 0.5/1 | Actual upstream filter, partial layout/map coverage. |
| Copy/mix routing | 1/1 | Actual upstream Copy with Linux PDC adapter; core tests. |
| GraphicEQ | 0.5/1 | Actual upstream DSP, optional FFTW3f and limited independent numeric/per-rate real-time coverage. |
| Convolution | 1/1 | The actual upstream `ConvolutionFilter`/`libHybridConv` DSP runs; same-rate and modulo-channel behavior are inherited upstream semantics. Error-policy differences are counted under config/runtime, not DSP math. |
| LoudnessCorrection | 0.5/1 | Upstream DSP; Linux volume provider only supplies representable uniform default-sink gain. |
| Mono/stereo runtime DSP | 1/1 | Mono/stereo deterministic PipeWire E2E. |
| >2-channel realtime mapping | 0.5/1 | Mapping/code exists for common positions, but hardware/E2E coverage is incomplete. |
| Sample-rate-dependent setup | 1/1 | 44.1/48/96 kHz E2E registrations and Engine initialization; test selection varies by build. |
| Block-size/quantum reconfigure | 1/1 | Reconfigure/format tests; convolution exception remains. |
| Multi-node PDC/alignment correctness | 1/1 | Delay/copy/plugin fixtures and PDC tests, not every possible route graph. |
| **DSP PARITY** | **19/21 = 90.5%** | Partial capped at half credit. Test coverage is tracked separately from implementation status. |

Preamp observation: `PreampFilterFactory.cpp` parses dB and constructs the actual upstream `PreampFilter`; its `process()` multiplies each channel by `pow(10, dB/20)`. Linux `Engine::process()` dispatches its `FilterConfiguration` and Runtime writes that processed buffer to virtual output. The focused core and PipeWire tests passed and assert −6 dB → `0.501187`. Therefore current evidence does **not** support “Preamp DSP is broken.” The UI document and runtime are separate states: Linux editor shows the open/possibly unsaved file; daemon status reports its active config path/filter chain. If the displayed value is unsaved or daemon has not accepted/reloaded that file, a stale/other runtime graph can report ratio 1. If status simultaneously confirms the exact active `Preamp: -6 dB`, non-silent input, and steady-state ratio 1, that would contradict the tests and merits a captured reproduction with config path, filter chain, status JSON and independent virtual-source recording. No such exact incident package was available in this audit, so the specific observation cannot be reproduced or conclusively attributed here.

## Config/runtime wiring and stage semantics

`Engine::loadConfig()` creates a candidate list, parses supported commands and initializes upstream filter factories; `Engine::process()` wraps the interleaved input into the upstream channel-pointer process adapter and processes in place. `Runtime::process()` maps PipeWire planar buffers to preallocated interleaved scratch, processes the active Engine (or crossfade), maps back to output and updates stats. The stats RMS/ratio are accumulated over processed samples and are not a plugin/filter parser trace. A nonzero Preamp displayed by the editor alone does not prove the daemon loaded it.

Notable semantic deltas:

- Upstream `FilterEngine` factory list includes Device/If/Expression/Include/Stage and EAPO Loudness/VST. Linux custom parser directly handles controls and only registers filter factories that have Linux implementations/build dependencies.
- EAPO `deviceName`, `connectionName` and `stage` now have MuParserX mappings; `connectionName` maps best-effort to PipeWire `node.name`, and string constants are unavailable in the classic muParser fallback. `deviceGuid` and direct Registry functions are N/A_WINDOWS with config-migration impact.
- Linux recognizes pre-mix/post-mix names but only capture stage can activate filters in the current graph.
- `outputChannelCount` is assigned the input/capture count; EAPO can receive independent output-channel topology.
- Linux rejects unsupported/malformed config and preserves prior graph. Some upstream paths report/log and continue or turn unknown factories into no-op, so “same parser behavior” is not absolute.
- Linux `Convolution` loads IR with libsndfile, insists on sample-rate match, uses modulo IR-channel selection and requires fixed blocks; upstream loader logs an IR-rate mismatch and keeps its native Windows file/path choices.

## Plugin parity in practical workflow

Backend: native LV2, CLAP, VST3; optional opt-in Linux VST2 through FST. Config: Linux-added `Plugin: <format> ...`; the older `VSTPlugin:` line is a restricted compatibility form, not Windows VST compatibility. CLI: `plugin list/scan/info/set/bypass` and catalog output. GUI: Add Filter/Tools can select a discovered LV2/CLAP/VST3 entry, insert a directive and edit writable config parameter values; this is not live readback or a state/native-GUI editor, and FST/VST2 remains manual-only. Realtime: plugin instances are in daemon process, with format hosts and tests for parameter, bypass/failure/latency/state behavior; CLAP `CLAP_PARAM_IS_BYPASS`, VST3 `ParameterInfo::kIsBypass`, and LV2 `lv2:enabled` are synchronized when advertised, while FST/VST2 and plugins without those controls use host dry bypass. Config reload builds candidate plugin graph away from callback and transitions/rebuilds; arbitrary third-party plugin crashes remain able to take down daemon. No yabridge/Wine management or Windows-plugin guarantee exists. In-process execution is a stability risk but not an EAPO port gap: upstream EAPO's VST host is also in-process.

## Editor UI source inventory and parity

Upstream `Editor/MainWindow.ui/.cpp` exposes File (New/Open/Save/Save As/Exit), Edit (Cut/Copy/Paste/Delete/Select All), View (Toolbar/Analysis panel), Settings (Language, reset global/file-specific preferences), an Add Filter menu via `IFilterGUIFactory`, tabs/recent config behavior, optional instant mode, device and channel configuration selectors, startup install/registry warnings, filter rows with add/remove/raw-text toggles, clipboard and drag/drop reorder/copy, and analysis panel. Widget factories cover BiQuad, Channel, Comment, Convolution, Copy, Delay, Device, Expression, GraphicEQ, Include, IIR, LoudnessCorrection, Preamp, Stage, VST. Additional dialogs include Channel/Copy graphs, Device pattern selector, Include/Convolution file selection, LoudnessCorrection and VST plugin editors. Translation catalogs exist under upstream Editor.

Linux `src/ui/MainWindow.cpp` is a separate compact single-document window that reuses selected upstream widget factories and `FilterTableRow`. It exposes Open, Save, Save As, Save & Check, Save & Reload, Add Filter; input device refresh/select; daemon start/stop/status; recent files; installed-plugin catalog, insertion picker and a config-value editor for writable LV2/CLAP/VST3 parameters; About; validation diagnostics/open error location; response-analysis button. It stores geometry/window state and recent files. Rows support insert/remove, selection/multiselect delete, Alt+Up/Down reorder, text fallback and byte-preserving edits. It does **not** expose the whole upstream menu/preferences/tabs, drag/drop/clipboard model, EAPO Device dialog, plugin state editor or native plugin GUI, LoudnessCorrection editor or translation picker. Parameter editor values are config overrides, not live plugin readbacks. Linux calls many Qt `tr()` labels but has no translation catalogs. GraphicEQ has an interactive plot; other rows do not all have an upstream-equivalent graph.

UI parity percentage is based on 44 enumerated user-workflow atoms; PASS=1, PARTIAL=0.5, BACKEND_ONLY=0.75, UI_ONLY=0.25, MISSING=0:

| UI atom group | Count | Earned |
|---|---:|---:|
| Open, save, save-as, recent, validate, reload (PASS/PASS/PASS/PARTIAL/PASS/PASS) | 6 | 5.5 |
| Preamp, biquad, IIR, Delay, Channel, Copy, GraphicEQ, Convolution, Include, Stage (PASS/PASS/PASS/PASS/PARTIAL/PARTIAL/PASS/PARTIAL/PARTIAL/PARTIAL) | 10 | 7.5 |
| Device selection, daemon status, daemon controls, row operations, multiselect, raw-line preservation, Device-command editor, troubleshooting (PARTIAL/PASS/REPLACED/PARTIAL/PARTIAL/PARTIAL/PARTIAL/PARTIAL) | 8 | 5 |
| Plugin catalog, native insertion, VST2 insertion, parameter editor, native UI (PARTIAL/PASS/MISSING/PARTIAL/MISSING) | 5 | 2 |
| Analysis magnitude, channel curves, source selection (`config.txt`/current file), resolution, latency, CPU, init time, peak gain, plugin response (PARTIAL/PASS/PASS/PARTIAL/MISSING/MISSING/MISSING/MISSING/MISSING) | 9 | 3 |
| Menus, tabs, preferences, localization, shortcuts, theme (PARTIAL/MISSING/PARTIAL/MISSING/PARTIAL/PASS) | 6 | 2.5 |
| **EDITOR UI PARITY** | **44** | **26/44 = 59.1%** |

This measures the listed workflows equally, not pixel similarity. Linux reuse of selected EAPO widgets is genuine but does not imply the whole EAPO editor is ported. Upstream `AnalysisPlotScene::setFreqData()` calculates magnitude with `sqrt(real² + imag²)`; this pinned EAPO editor does **not** expose phase or impulse-response waveform plots, so those were inspected but correctly excluded from the parity denominator.

## Device Selector/Configurator parity

The pinned EAPO tree has the current `DeviceSelector/DeviceSelector.cpp/.ui`; historical docs refer to the older `Configurator.exe`, but there is no separate current `Configurator/` source target in this revision. The Device Selector enumerates render/capture endpoints, connectors, default status, APO enabled/installed state, and install-mode options (LFX/GFX, SFX/MFX/EFX, use-original-APO and silent-buffer behavior). Its test dialog tests installation state on endpoints. Linux's combo + CLI selects **one capture source** by stable PipeWire property when available (otherwise `node.name`) and daemon status shows graph links/format; it cannot enable a Windows APO, choose a system-wide playback insertion stage or report Windows registry/install state. `Device:` is a config predicate over current selected source identity, not the same role as the EAPO Device Selector selection. A dedicated standalone Linux selector is not established as necessary by source evidence: current editor combo/CLI covers the basic single capture selection, while functional gaps are richer endpoint metadata and an interactive test/troubleshooting workflow, not merely a separate window.

## Response analysis vs realtime diagnostics

EAPO `AnalysisThread` initializes/processes configuration against a selected device/channel mask and exposes frequency data, latency, initialization time, processing time/CPU and peak gain to `AnalysisPlotScene` and the analysis dock (including selected channel and resolution). Linux `ResponseAnalysis::analyzeConfigResponse()` computes a theoretical per-input/per-output-channel magnitude transfer by processing an impulse through Engine and FFTing the resulting impulse. It intentionally disables plugin loading. The UI does not plot phase, impulse response, group delay, per-filter contributions or measured live response. Runtime status RMS/ratio, callback average/max, overrun counts, graph links and reported plugin latency are **live diagnostics**, not theoretical response analysis and not end-to-end latency measurement.

## Runtime lifecycle, reliability and unsupported semantics

| Concern | Evidence-based state |
|---|---|
| Capture → DSP → virtual mic | Implemented and independently consumed by existing native PipeWire E2E fixtures; prior 0.9 evidence is recorded in `docs/REALTIME.md`. This audit-time focused E2E used a private graph. |
| PipeWire disconnect/restart | Recovery paths and private server restart test exist. |
| Selected node removal/reappearance | Private-graph tests cover synthetic remove/recreate; physical hotplug hardware matrix is not proven. |
| Device switch | E2E fixture exists and is wired; does not add playback processing. |
| Rate/quantum renegotiation | Runtime signals control loop and rebuilds Engine; E2E test registrations exist. Convolution enforces fixed quantum/rate restrictions. |
| Mono/stereo | Explicit E2E coverage. |
| Multichannel | Common channel enum mapping/code exists; full real hardware and matrix routing coverage remains a gap. |
| Config reload | Inotify watcher builds/validates candidate off callback; failed reload retains last valid graph; tests exist. |
| Crash recovery | The supplied systemd user unit restarts a daemon killed by process failure and has an optional private-PipeWire SIGKILL/restart E2E; manually launched daemons are not supervised, and plugin process isolation is absent. |
| Realtime allocation | Preallocated work buffers and callback audit; tests assert zero tracked callback alloc/free. Shared-library internals are explicitly outside allocation audit scope. |
| Latency | Fixed Delay and plugin-reported latency/PDC have tests. Reported plugin latency sum is not end-to-end latency; PipeWire buffering, hardware/device and application capture latency are not combined into one measured number. |

## Packaging / integration parity

Linux has source definitions for Arch, DEB, RPM, generic tar and Nix, CMake install rules, user service, metadata, notices and package smoke scripts. This is a legitimate replacement of Windows NSIS/DLL/registry installation, not a port of it. EAPO setup includes architecture-specific install/uninstall, service/registry configuration, device integration and UpdateChecker, which have no literal Linux counterpart. Existing release package tests demonstrate extraction/staged smoke and checksums, but source parity cannot prove clean distro install/uninstall/update on every supported distribution.

## Test coverage gap map

| Area | Linux evidence available | Gap / what test does not prove |
|---|---|---|
| Preamp, IIR, biquad, Delay, Copy, Channel | `skyapo-core`, renderer/config tests | Core numerical/parser test is not a real microphone test; subtype-specific golden frequency response is not exhaustive. |
| GraphicEQ | build-gated core/render/UI/response coverage | No per-node golden magnitude suite across sample rates and active PipeWire path. |
| Convolution | core/offline/realtime fixtures | No exhaustive IR format/channel/rate/resampling/memory-bound matrix; same-rate limitation is intentional today. |
| Include/conditional/Device/Stage | parser/core/config-source tests and selected E2E, including nested Device/Stage Include-scope numerical regression | No exhaustive differential suite running identical nested fixtures through upstream and Linux parsers; remaining syntax corners are not all covered. |
| PipeWire routing/restart/reconfigure | `pipewire_e2e_test.py` and native fixtures | Private graph is not a hardware/device-vendor matrix; does not prove every SPA position or app/client. |
| Plugin | LV2/CLAP/VST3/VST2 in-tree test plugins, failure/state/latency/realtime fixtures | Fixture behavior is controlled and cannot establish arbitrary third-party reliability, GUI integration, or daemon crash isolation. State persistence has explicit round-trip fixture coverage for all four formats, including `tests/lv2_live_control_test.cpp`; arbitrary third-party state compatibility remains unproven. |
| UI | `skyapo-ui-editor`, resource audit tests | No automated interactive visual/accessibility/shortcut test covering every menu or actual plugin-user workflow. |
| Response analysis | UI test compares selected Preamp+biquad numerical response | No group-delay or plugin response tests; plugins intentionally unsupported in theoretical analysis. Phase/IR plots are absent upstream and are not parity criteria. |
| Package/install | smoke tests and CTest packaging checks | A clean OS install, transaction rollback, upgrade/uninstall and user-preserved config behavior remain distro/environment-specific. |
| Upstream differential parity | two upstream portable config fixtures are run by `skyapo-upstream-config-compatibility` | It is only two positive fixtures (`iir_lowpass.txt`, `selective_delay.txt`) plus explicit rejection of four Room EQ Wizard exports, not whole-tree EAPO parity. |

Upstream contains `Benchmark/Benchmark.cpp`, but no broad upstream DSP unit-test suite is evident in this checkout. SkyAPO's tests therefore provide stronger Linux runtime regression evidence for selected cases, not an upstream differential proof.

## Quantitative summaries

Metrics are independent scorecards, not percentages of source lines/classes. Exact status weights per one-point atom are: PASS = 1.00/1; PARTIAL = 0.50/1; BACKEND_ONLY = 0.75/1; UI_ONLY = 0.25/1; BROKEN = 0/1; MISSING = 0/1; REPLACED = 1.00/1 only when the Linux replacement fulfills the same scoped role (otherwise it earns 0 and should generally be classified PARTIAL/MISSING instead); N/A_WINDOWS = excluded from both earned and maximum points. No single overall percentage is published because the dimensions have different user impact and combining them would require an arbitrary weighting.

The exact status weights are: PASS = 1.00; PARTIAL = 0.50; BACKEND_ONLY = 0.75; UI_ONLY = 0.25; BROKEN = 0; MISSING = 0; REPLACED = 1.00 only when the Linux substitute fulfills the same scoped role (otherwise it is not awarded as equivalent); N/A_WINDOWS = excluded from numerator and denominator. Each scored atom has maximum 1 point. These weights are an explicit audit convention, not upstream-defined values.

| Dimension | Earned / applicable atoms | Percentage | Notes |
|---|---:|---:|---|
| DSP | 19 / 21 | **90.5%** | Scorecard above; partial credit capped at half. No test-coverage penalty is applied where source and runtime wiring exist. |
| Config language | 32 / 43 | **74.4%** | 43 portable/applicable upstream EAPO atoms; Linux-added `Plugin:` and Windows Registry/GUID atoms excluded. Output-channel variable, device-string/stage fallback availability and optional GraphicEQ support receive partial credit. Capture is supported; pre-mix and post-mix are distinct unimplemented placements. |
| Realtime/runtime | 21 / 24 | **87.5%** | 24 implementation capabilities: capture path, independent virtual-source consumer, F32, rate, quantum, mono, stereo, multichannel, live switch, synthetic hotplug, server restart, hot reload, rollback, callback allocation, bounded PDC, plugin-latency refresh, timing, daemon lifecycle, crash supervision, shutdown, playback, stable selection, user service, graph crossfade. Points: 1×7 + .5 multichannel +1 switch +1 hotplug +1×6 server restart/reload/rollback/allocation/PDC/latency refresh +.5 timing +1 daemon +1 crash supervision +1 shutdown +0 playback +.5 stable selection +.5 user service +1 crossfade =21. A physical-device test matrix is reported as confidence coverage, not counted as a capability. |
| Editor UI | 26 / 44 | **59.1%** | Document workflows 5.5/6; filter editors 7.5/10 (Stage editor is PARTIAL: capture selection works, pre/post are disabled); device/status/row workflows 5/8; plugin workflows 2/5; response-analysis workflows 3/9; menus/preferences 2.5/6. Exact included rows and arithmetic are below. |
| Device management | 7 / 12 | **58.3%** | Enumeration, source/output distinction, default metadata, stable identity, selection, Device predicate, format metadata, interactive test, live switch, playback selection, capture-source hotplug, active support status. In order: PARTIAL, PARTIAL, PARTIAL, PARTIAL, PARTIAL, PARTIAL, PASS, PARTIAL, PASS, MISSING, PASS, PARTIAL. Arithmetic: .5+.5+.5+.5+.5+.5+1+.5+1+0+1+.5=7/12. Daemon restart/reload and generic service supervision are runtime/lifecycle capabilities, not Device Selector capabilities, and are excluded. |
| Plugin | 23.25 / 29 | **80.2%** | Atoms: backend formats (4), legacy VST compatibility (1), discovery (4), config wiring (4), parameter control (4), runtime state persistence (4), runtime latency/PDC (4), editor capabilities (4). Points: backend LV2/CLAP/VST3 PARTIAL×3=.5×3 and VST2 BACKEND_ONLY=.75 gives 2.25; legacy PARTIAL=.5; discovery=3; config=4; parameter control=4; runtime state=4; latency/PDC=4; UI native insertion PASS=1, VST2 insertion MISSING=0, parameter editor PARTIAL=.5, native GUI MISSING=0. Sum=23.25/29. Linux-host feature score, not direct EAPO format parity. |
| Analysis tooling | 3 / 9 | **33.3%** | Plugin-free magnitude response PARTIAL=.5; per-channel curves PASS=1; config source selection PASS=1; FFT resolution PARTIAL=.5; latency, CPU, initialization time, peak gain, plugin-inclusive response are MISSING=0. Arithmetic: .5+1+1+.5+0+0+0+0+0=3/9. Upstream does not show phase/IR plots; live diagnostics are not theoretical analysis. |
| Packaging/integration | 8 / 8 | **100.0%** | Implementation atoms: CMake install, Arch, DEB, RPM, generic archive, Nix, user service, package-manager upgrade/uninstall lifecycle. Points 1×7 + REPLACED lifecycle 1 =8. A dependency-populated clean-install run is test confidence/evidence, not an implementation capability, so it is not scored as zero. EAPO auto-update is excluded as Windows-specific. |

### Audited score ledger

Each scored atom has maximum 1 point. Status weights are exactly: PASS=1; PARTIAL=0.5; BACKEND_ONLY=0.75; UI_ONLY=0.25; BROKEN=0; MISSING=0; REPLACED=1 only where the Linux substitute fills the same role; N/A_WINDOWS=excluded (no denominator point). A test gap is not a status weight: status describes source/runtime implementation, while evidence confidence is separate. Domain atoms intentionally overlap: the corrected eight denominators sum to 190, not 105 matrix entries, because e.g. Preamp appears as DSP math, config syntax, UI exposure and realtime execution. These percentages must not be summed into one whole-project score.

| Domain | Scored rows: `capability [status] = awarded/max` | Arithmetic |
|---|---|---|
| DSP (14 grouped rows, 21 atoms) | Preamp PASS 1/1; biquad `PK/PEQ/Modal`, `LP/LPQ`, `HP/HPQ`, `BP`, `LS/LSC`, `HS/HSC`, `NO`, `AP` each PASS 1/1 (8/8); IIR PASS 1/1; Delay PASS 1/1; Channel PARTIAL .5/1; Copy PASS 1/1; GraphicEQ PARTIAL .5/1; Convolution DSP math PASS 1/1; LoudnessCorrection PARTIAL .5/1; mono/stereo runtime PASS 1/1; >2-channel mapping PARTIAL .5/1; sample-rate setup PASS 1/1; block/quantum setup PASS 1/1; multi-node PDC PASS 1/1. | 1+8+1+1+.5+1+.5+1+.5+1+.5+1+1+1 = **19/21 = 90.5%**. |
| Config language (43 applicable atoms) | Preamp PASS 1/1; biquad syntax subtypes `PK/PEQ/Modal`, `LP/LPQ`, `HP/HPQ`, `BP`, `LS/LSC`, `HS/HSC`, `NO`, `AP` each PASS 1/1 (8/8); IIR PASS 1/1; GraphicEQ PARTIAL .5/1; Convolution PARTIAL .5/1; Delay ms/samples PASS 2/2; Channel semantics/selectors PARTIAL 1/2; Copy PASS 1/1; Device patterns PARTIAL .5/1; Stage capture PASS 1/1, pre-mix MISSING 0/1, post-mix MISSING 0/1; Include PASS 1/1; If/ElseIf/Else/EndIf PASS 4/4; Eval/backticks PARTIAL 1/2; regex functions PARTIAL 1/2; sampleRate/inputChannelCount PASS 2/2; outputChannelCount PARTIAL .5/1; deviceName/connectionName/stage PARTIAL 1.5/3; comments/quoted-hash PARTIAL .5/1 plus whitespace PASS 1/1; case/locale PARTIAL 1/2; LoudnessCorrection PARTIAL .5/1; legacy VSTPlugin PARTIAL .5/1; disabled `Filter N: OFF None` PASS 1/1. Excluded: deviceGuid N/A_WINDOWS 1; `readRegString`/`readRegDWORD` N/A_WINDOWS 2; Linux-only `Plugin:`. | 1+8+1+.5+.5+2+1+1+.5+1+0+0+1+4+1+1+2+.5+1.5+1.5+1+.5+.5+1 = **32/43 = 74.4%**. |
| Realtime/runtime (24 rows) | Capture path REPLACED 1/1; independent virtual-source consumer REPLACED 1/1; F32 PASS 1/1; sample-rate negotiation PASS 1/1; quantum renegotiation PASS 1/1; mono PASS 1/1; stereo PASS 1/1; multichannel PARTIAL .5/1; live switch PASS 1/1; synthetic hotplug PASS 1/1; PipeWire restart PASS 1/1; config hot reload REPLACED 1/1; bad-config rollback PASS 1/1; callback allocation audit PASS 1/1; bounded PDC PASS 1/1; dynamic plugin-latency refresh PASS 1/1; processing timing PARTIAL .5/1; daemon lifecycle REPLACED 1/1; process-crash supervision PASS 1/1; clean shutdown PASS 1/1; playback path MISSING 0/1; stable selection PARTIAL .5/1; user-service supervision PARTIAL .5/1; graph crossfade PASS 1/1. | 7 +.5+1+1+6+.5+1+1+1+0+.5+.5+1 = **21/24 = 87.5%**. Physical hardware matrix is not an implementation atom. |
| Editor UI (6 groups, 44 atoms) | Open/save/save-as/recent/validate/reload: PASS/PASS/PASS/PARTIAL/PASS/PASS = 5.5/6; Preamp/biquad/IIR/Delay/Channel/Copy/GraphicEQ/Convolution/Include/Stage editors = PASS/PASS/PASS/PASS/PARTIAL/PARTIAL/PASS/PARTIAL/PARTIAL/PARTIAL = 7.5/10; device/status/controls/row/multiselect/raw/Device/troubleshooter = PARTIAL/PASS/REPLACED/PARTIAL/PARTIAL/PARTIAL/PARTIAL/PARTIAL = 5/8; plugin catalog/native insertion/VST2 insertion/parameter editor/native UI = PARTIAL/PASS/MISSING/PARTIAL/MISSING = 2/5; analysis magnitude/channel/source-choice/resolution/latency/CPU/init/peak/plugin response = PARTIAL/PASS/PASS/PARTIAL/MISSING/MISSING/MISSING/MISSING/MISSING = 3/9; menus/tabs/preferences/localization/shortcuts/theme = PARTIAL/MISSING/PARTIAL/MISSING/PARTIAL/PASS = 2.5/6. | 5.5+7.5+5+2+3+2.5 = **26/44 = 59.1%**. |
| Device management (12 rows) | Enumeration PARTIAL .5/1; source/output distinction PARTIAL .5/1; default metadata PARTIAL .5/1; stable identity PARTIAL .5/1; selection PARTIAL .5/1; Device predicate PARTIAL .5/1; format metadata PASS 1/1; interactive test PARTIAL .5/1; live switch PASS 1/1; playback selection MISSING 0/1; capture-source hotplug PASS 1/1; active support status PARTIAL .5/1. | .5+.5+.5+.5+.5+.5+1+.5+1+0+1+.5 = **7/12 = 58.3%**. |
| Plugin (29 atoms) | Four format backends: LV2/CLAP/VST3 PARTIAL .5/1 each, VST2 BACKEND_ONLY .75/1 = 2.25/4; legacy VSTPlugin PARTIAL .5/1; discovery LV2/CLAP/VST3 PASS 3/3 and VST2 MISSING 0/1; config-directive wiring PASS 4/4; parameter control PASS 4/4; runtime state PASS 4/4; latency/PDC PASS 4/4; UI native-format insertion PASS 1/1, FST/VST2 insertion MISSING 0/1, parameter editor PARTIAL .5/1, native GUI MISSING 0/1. | 2.25+.5+3+4+4+4+4+1.5 = **23.25/29 = 80.2%**. Backend rows score host/runtime implementation; config-directive wiring is a distinct parser/factory integration capability, not a second count of the host itself. VST2/FST is opt-in and has no scanner/catalog listing. All four state backends have fixture round-trip coverage; arbitrary third-party behavior remains unproven. |
| Analysis tooling (9 rows) | Plugin-free magnitude PARTIAL .5/1; per-channel curves PASS 1/1; analysis source (`config.txt`/current file) PASS 1/1; resolution PARTIAL .5/1; analysis latency MISSING 0/1; CPU MISSING 0/1; init time MISSING 0/1; peak gain MISSING 0/1; plugin-inclusive response MISSING 0/1. | .5+1+1+.5+0+0+0+0+0 = **3/9 = 33.3%**. |
| Packaging/integration (8 rows) | CMake install PASS 1/1; Arch PASS 1/1; DEB PASS 1/1; RPM PASS 1/1; generic archive PASS 1/1; Nix PASS 1/1; user service PASS 1/1; package-manager upgrade/uninstall REPLACED 1/1. | 1×7+1 = **8/8 = 100.0%** for implementation coverage only; clean-install matrix remains a confidence/test item. |

The table above is the auditable percentage ledger. The corrected domain maximums add to 190 (21+43+24+44+12+29+9+8), not 97 or 105 matrix rows: denominators overlap because one user workflow can have separate DSP, config, runtime, UI, device, plugin and analysis capabilities. For example, a Preamp's DSP math, config syntax, editor widget and realtime execution are distinct scored questions. These domain denominators must not be summed into a count of upstream capabilities. The former 97-row snapshot is unavailable, while the corrected current matrix has 105 textual labels. Matrix-entry classification counts are a different inventory, and no overall parity percentage is calculated.

The config and device/runtime atom scorecards are capability indicators, not acceptance claims: an 87.5% runtime score means 21 weighted points on the 24 implementation-capability checklist, not that 87.5% of user audio passes correctly. Hardware-vendor coverage and format-specific test gaps are reported separately from implementation status.

## Power-user workflow outcomes

| Workflow | Result | Blocking detail |
|---|---|---|
| Select input device | Works | One PipeWire capture source, not arbitrary render/capture APO target. |
| Apply Preamp/parametric EQ | Works | Engine and independent-consumer evidence; editor/daemon config state must be synchronized. |
| Graphic EQ | Works with build dependency | FFTW3f needed; UI supported. |
| Convolution IR | Partial | Actual upstream hybrid DSP; matching rate, modulo channel mapping and fixed-block behavior are inherited EAPO semantics. The Linux-specific difference is invalid/mismatched-IR candidate rejection vs upstream pass-through/logging behavior, not absent resampling/matrix features. |
| Delay | Works | EAPO ms/sample syntax and PDC fixtures. |
| Channel routing/per-channel filters | Partial-to-works | Common channel map supported, but uncommon masks/multichannel input matrix incompletely tested. |
| Multiple configs/includes | Works with caveats | Relative/nested Include and reload supported; expression-based Save As rebase intentionally refuses unsafe transformation. |
| Device-specific config | Partial | `Device:` matches selected source string; not all EAPO device metadata/runtime vars or render devices. |
| Stage-specific config | Partial | Capture only; pre-mix/post-mix have no runtime graph. |
| VST/plugin chain | Partial | Linux plugin directives supported by backend; hand-edit config/CLI required. Legacy Windows `VSTPlugin:` is restricted. |
| Add plugin and edit its UI | Partial | Add Filter/Tools inserts discovered LV2/CLAP/VST3 directives and edits writable saved-config parameters; no live value readback, state panel or native plugin GUI. FST/VST2 remains manual-only. |
| Edit while audio is running | Works | Save+reload/watcher builds candidate off callback and swaps valid graph. |
| Inspect response graph | Partial | Theoretical magnitude only; no phase/IR plot, and less analysis metadata/control than EAPO. |
| Troubleshoot processing | Partial | Text diagnostics and CLI status; no interactive loopback Device Selector wizard. |
| Switch device/recover service | Partial | PipeWire switch/restart and systemd-managed process-crash restart are tested privately; physical hardware matrix remains incomplete. |

## 20 largest remaining gaps (ordered by functional risk/value)

1. No playback/sink processing path; only capture-to-virtual-mic.
2. No equivalent to EAPO's pre-mix/post-mix execution stages.
3. GUI can insert discovered LV2/CLAP/VST3 directives and edit writable saved-config parameter values, but lacks live parameter readback, state editing and native plugin GUI; FST/VST2 insertion remains manual.
4. Plugins execute in daemon process; a crashing plugin can kill audio service.
5. Existing configs using direct Windows Registry functions need migration; these are N/A_WINDOWS, not a portable implementation gap.
6. Device expression semantics are build-dependent: `connectionName` maps to `node.name` rather than a hardware connector, and the classic muParser fallback has no string constants.
7. `Stage: pre-mix/post-mix` is syntactically accepted but no-op in current capture path.
8. Device command is selected-capture substring matching, not EAPO endpoint metadata semantics.
9. Full multichannel channel masks/SPA positions and physical hardware coverage are incomplete.
10. No full Device Selector test wizard/loopback diagnostic.
11. Optional FST/VST2 has no scanner/catalog listing and remains behind legal and third-party support gates.
12. `outputChannelCount` expression constant aliases the input count instead of modeling independent output topology.
13. No EAPO-style estimated processing latency/CPU/init-time/peak-gain analysis panel.
14. Response analysis rejects/does not model third-party plugin responses.
15. Convolution mismatch handling differs: Linux rejects the candidate graph, while upstream logs and can leave the filter pass-through.
16. Add Filter omits Include, Expression and LoudnessCorrection templates; several can only be edited as raw config. A basic `Device:` match template exists and is not equivalent to the Device Selector.
17. LoudnessCorrection only receives representable uniform default-sink gain through the Linux provider.
18. Legacy VST compatibility cannot load Windows DLLs or fully preserve EAPO ChunkData.
19. No editor translation catalogs/language switching or full EAPO preference parity.
20. A manually launched daemon (or a user who has not enabled the supplied service) has no external supervisor; configured systemd user units do restart after process failure in the tested setup.

## Roadmap proposal from audit evidence

### 0.9.1 — correctness/regression fixes only

- Clarify editor unsaved-document state vs daemon active-config path in the UI/status, then capture the exact reported Preamp/RMS reproduction before changing DSP.
- Add differential parser fixtures for nested Device/Stage/Include scope, comments/escaping, numeric locales, fallback expression backend and legacy VST parsing.
- Resolve Qt/SPA minimum-version build declaration or compile guards surfaced by CI; repair Arch CI's checkout container lacking Git. These are build/test reliability gaps, not features.
- Add runtime tests for every supported SPA channel position and multichannel PCM layout; make unsupported mapping explicit at device selection.
- Add fail-fast diagnostics for inactive `Stage` values rather than suggesting they process audio.

### 0.10 — high-value user-facing parity

- Add an editor plugin parameter/state inspector (basic LV2/CLAP/VST3 directive insertion already exists; do not imply native GUI embedding).
- Extend analysis controls beyond the tested `config.txt`/current-file choice and supported 1024–131072 FFT sizes; add upstream latency/CPU/init/peak-gain metadata only where Linux can report it accurately.
- Add a dedicated interactive capture test/troubleshooter, config/device visual state and more explicit active-vs-edited config presentation.
- Expand EAPO expression/device compatibility where a stable Linux meaning exists (device string/stage); otherwise document precise migration mapping.
- Improve multichannel channel map selection and test support.

### 1.0 gates

**MUST HAVE:** repeatable independent-client PipeWire E2E on supported hardware/layout matrix; PDC/latency correctness bounded and documented; robust daemon recovery and validated last-good hot reload; clear unsupported config errors; clean installs/upgrades/uninstalls on declared distributions; all shipped plugin formats tested with representative third-party plugins and safe failure reporting; no critical realtime allocation/regression; Linux UI can expose every feature marketed as GUI-editable.

**SHOULD HAVE:** device loopback wizard; plugin state editor and live parameter readback; stronger multichannel UX; migration report for unsupported Windows directives; translation/language catalogs. Basic saved-configuration parameter editing for LV2/CLAP/VST3 exists, but does not expose active plugin values. Phase/IR plots may be useful additions, but are not EAPO parity requirements because the pinned EAPO editor does not provide them.

**OPTIONAL:** full plugin process sandbox/worker isolation, cross-platform plugin GUI embedding, complete editor feature parity, broad automated hardware matrix.

**N/A Windows-specific:** COM registration and `IAudioProcessingObject`/`IAudioSystemEffects`; Windows APO install modes, registry enablement and Protected Audio DG registry checks; Windows service installer/uninstaller and DLL architecture bundles; Windows endpoint GUID registry functions where no portable contract exists; Windows HWND VST editor embedding. These should not be recreated literally on Linux.

## Source pointers

- Upstream source authority/revision: `upstream/equalizerapo` gitlink; `FilterEngine.cpp`, `FilterConfiguration.cpp`, `filters/*`, `parser/*`, `Editor/*`, `DeviceSelector/*`, `Benchmark/*`, `Setup/*`.
- Linux DSP/parse/runtime: `src/core/Engine.cpp`, `src/platform/linux/UpstreamFilterEngineProcess.h`, `src/platform/linux/ConfigSource.cpp`, `src/pipewire/Runtime.cpp`.
- Linux editor: `src/ui/MainWindow.cpp`, `src/ui/ResponseAnalysis.cpp`, `src/ui/ConfigFile.cpp`, `src/ui/ChannelCopyEditor.cpp`, `src/ui/ConvolutionEditor.cpp`, `src/ui/IIRFilterEditor.cpp`, `src/ui/IncludeEditor.cpp`, `src/ui/upstream/FilterTable.h`.
- Linux devices/control: `src/pipewire/DeviceManager.cpp`, `src/pipewire/DeviceIdentity.h`, `src/cli/main.cpp`, `src/daemon/main.cpp`, `src/platform/Settings.h`.
- Test suites: `tests/core_tests.cpp`, `tests/render_tests.cpp`, `tests/pipewire_e2e_test.py`, `tests/ui_tests.cpp`, `tests/realtime_safety_tests.cpp`, `tests/upstream_config_compatibility_test.py`, `tests/config_watcher_test.cpp`, plugin fixtures under `tests/plugins/`.
