# UI port audit and status

There is no SkyAPO GUI target yet. The pinned upstream `Editor/Editor.pro` is a Qt Widgets application (`core`, `gui`, `widgets`) built from a real configuration editor, filter-row/factory system, specialized filter widgets, custom Qt graphics widgets, and response-analysis views/worker. These are useful source assets and are preferable to copying the editor's appearance in an unrelated UI.

## Reuse candidates

- `Editor/FilterTable*`, `FilterTemplate*`, `IFilterGUI*` and `IFilterGUIFactory*` implement the ordered configuration/filter-row model and plugin-like per-directive editor registration.
- The Preamp, BiQuad, Delay, Channel, Copy, Include, GraphicEQ and Convolution GUI classes provide concrete controls and serialize EAPO directives. They should be ported with their upstream notices intact, then enabled only for directives SkyAPO actually validates.
- `Editor/widgets/FrequencyPlot*`, `GraphicEQFilterGUI*`, `AnalysisPlot*` and `AnalysisThread*` contain reusable Qt scene/view and response-analysis behavior. Their actual input/calculation contract must be reconciled with SkyAPO's active graph before presenting a response as authoritative.
- `Editor/FilterTableRow.ui`, `MainWindow.ui`, per-filter `.ui` resources, translations and Qt resources preserve the original interaction/layout vocabulary.

## Windows integration that must be replaced

`MainWindow.cpp` discovers input/output `DeviceAPOInfo`, checks APO registration and Windows enhancement state, offers repair/install prompts, and contains `ShellExecuteW(..., "runas")` elevation, `CreateFile`/Win32 error handling, and registry-backed configuration/settings paths. `main.cpp` obtains the config directory and language from the Registry and writes editor state through `QSettings::NativeFormat`. Device filter GUIs operate on `AbstractAPOInfo`/`DeviceAPOInfo` and Windows channel masks. The VST GUI selects Windows `.dll` files and performs Local Service ACL changes. These are not portable UI details: the Linux window must query/control `skyapod` (stable PipeWire device identity, negotiated format and virtual source), use XDG paths, and delegate all plugin behavior to the daemon/host.

## Port boundary

Keep the daemon as owner of PipeWire, DSP, config validation/reload and plugins. The GUI is an IPC client and closing it must not stop audio. Port the upstream filter-table/editor widgets and response visualizations incrementally where their config semantics match SkyAPO; replace the Windows `MainWindow` shell, device/APO setup, registry persistence and VST DLL UI with Linux/daemon-backed implementations. Preserve hand-written config/comments where practical, and never claim the displayed response matches runtime until it is computed from the active graph.

The current development environment has Qt 6 Widgets 6.11.2 available, but the upstream project is an older qmake project with Windows-specific linker flags and hard-coded `C:/Program Files` paths for libsndfile, FFTW and muparserX; no Qt port has been compiled yet. A Qt version/build-system compatibility pass and separate GUI/resource dependency-license audit are still required. No GUI behavior is currently implemented or tested.
