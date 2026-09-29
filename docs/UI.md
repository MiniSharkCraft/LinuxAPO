# UI port audit and status

The first SkyAPO GUI target, `skyapo-ui`, builds with Qt 6 Widgets. The pinned upstream `Editor/Editor.pro` is a Qt Widgets application (`core`, `gui`, `widgets`) built from a real configuration editor, filter-row/factory system, specialized filter widgets, custom Qt graphics widgets, and response-analysis views/worker. The current target directly compiles selected upstream GUI source files from the submodule; it does not copy/rewrite those controls.

## Reuse candidates

- `Editor/FilterTable*`, `FilterTemplate*`, `IFilterGUI*` and `IFilterGUIFactory*` implement the ordered configuration/filter-row model and plugin-like per-directive editor registration.
- The Preamp, BiQuad, Delay, Channel, Copy, Include, GraphicEQ and Convolution GUI classes provide concrete controls and serialize EAPO directives. They should be ported with their upstream notices intact, then enabled only for directives SkyAPO actually validates.
- `Editor/widgets/FrequencyPlot*`, `GraphicEQFilterGUI*`, `AnalysisPlot*` and `AnalysisThread*` contain reusable Qt scene/view and response-analysis behavior. Their actual input/calculation contract must be reconciled with SkyAPO's active graph before presenting a response as authoritative.
- `Editor/FilterTableRow.ui`, `MainWindow.ui`, per-filter `.ui` resources, translations and Qt resources preserve the original interaction/layout vocabulary.

## Windows integration that must be replaced

`MainWindow.cpp` discovers input/output `DeviceAPOInfo`, checks APO registration and Windows enhancement state, offers repair/install prompts, and contains `ShellExecuteW(..., "runas")` elevation, `CreateFile`/Win32 error handling, and registry-backed configuration/settings paths. `main.cpp` obtains the config directory and language from the Registry and writes editor state through `QSettings::NativeFormat`. Device filter GUIs operate on `AbstractAPOInfo`/`DeviceAPOInfo` and Windows channel masks. The VST GUI selects Windows `.dll` files and performs Local Service ACL changes. These are not portable UI details: the Linux window must query/control `skyapod` (stable PipeWire device identity, negotiated format and virtual source), use XDG paths, and delegate all plugin behavior to the daemon/host.

## Current implementation

The Linux shell includes an XDG-default config path, Open/Save, Save & Check, Save & Reload, daemon status/start/stop, input device enumeration/selection, an add-filter menu, row removal, and inline text editing for directives without an implemented visual editor. Existing `Preamp:` and parametric `Filter:` lines instantiate the actual upstream Preamp and BiQuad widgets/factories; their editors serialize edits back to those directives. `ConfigFile` retains untouched line bytes and line endings so comments, includes, whitespace and unsupported-but-preserved rows are not reformatted unless edited. Save uses `QSaveFile`; closing with unsaved changes prompts.

The process is a daemon client and invokes the existing `skyapo` CLI for status, start/stop, device selection, config validation and reload. This keeps the audio graph in `skyapod`; control calls are outside realtime. The current MVP uses synchronous short-lived CLI calls on the GUI thread and can briefly wait on device listing/config checks; asynchronous UI-side request handling is a follow-up.

It currently has no plugin editor, channel/Copy/Include/GraphicEQ/Convolution visual rows, response curve, syntax highlighting, recent-file/preferences model, translation integration, or comprehensive GUI automation. Unsupported lines remain editable as raw text and are not presented as valid. The device/status presentation is the CLI's actual response, not simulated state.

## Port boundary

Keep the daemon as owner of PipeWire, DSP, config validation/reload and plugins. The GUI is an IPC client and closing it must not stop audio. Port the upstream filter-table/editor widgets and response visualizations incrementally where their config semantics match SkyAPO; replace the Windows `MainWindow` shell, device/APO setup, registry persistence and VST DLL UI with Linux/daemon-backed implementations. Preserve hand-written config/comments where practical, and never claim the displayed response matches runtime until it is computed from the active graph.

The development environment has Qt 6 Widgets 6.11.2. The upstream project remains an older qmake project with Windows-specific linker flags and hard-coded `C:/Program Files` paths for libsndfile, FFTW and muparserX. SkyAPO avoids those settings and currently builds a selected subset with CMake; the rest of the upstream UI still needs a Qt 6 compatibility pass and separate GUI/resource dependency-license audit.
