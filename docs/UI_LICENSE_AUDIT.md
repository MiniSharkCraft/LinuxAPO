# Qt editor license/resource audit

Audit performed against the current CMake target, pinned official
`upstream/equalizerapo` checkout, staged-install rules and the Linux runtime
linker output. This is an engineering inventory, not legal advice.

## Selected UI code

`skyapo-eapo-editor` directly compiles a subset of `upstream/equalizerapo/Editor`
using Qt 6 Widgets: `FilterTableRow`, `CompactToolBar`, `EscapableLineEdit`,
`IFilterGUI`, `IFilterGUIFactory`, `FilterTemplate`, `GUIHelper`, and the
Preamp/BiQuad/Delay/Stage widgets and factories. When FFTW3f is available it
also builds the upstream GraphicEQ GUI/plot widgets. These source files carry
the upstream `Editor/Licenseheader.txt` notice (GPL v2 or, at the recipient's
option, any later version); their headers are preserved and the upstream
`License.txt` is installed unchanged.

The full Windows `Editor.qrc` is not compiled. It included resources unrelated
to the selected Linux widgets, including Editor/Qt-base translation catalogs
and `sounds/pinkNoise.flac` used by the excluded Windows-dependent loudness
calibration UI. Two curated qrc files expose only the icon paths referenced by
the selected code. Their payloads are project-authored SVG icons under
`src/ui/resources/icons/`, replacing upstream artwork with unverified
individual authorship/license:

- Always: `list-add-green.ico`, `list-remove-red.ico`,
  `accessories-text-editor.ico`.
- With FFTW3f/GraphicEQ: `resize_corner.ico` and the three dark-mode response
  icons (`invert`, `normalize`, `reset`).

The `.ico` aliases preserve paths expected by unmodified upstream UI source,
while the embedded payloads are the new SVG files. The pinned upstream tree has
no per-icon author/license metadata; none of its icon artwork is now
redistributed by these qrc files. `skyapo-ui-resource-audit` verifies qrc paths
against selected source references, ensures payloads are project-authored SVGs,
and rejects unused translation/audio resources.

## Qt runtime dependency

The editor links `Qt6::Widgets`; its runtime dependencies are the host's Qt 6
Core/Gui/Widgets shared libraries (verified locally with `ldd build/skyapo-ui`).
SkyAPO does not bundle Qt binaries. Qt's official Qt 6.11 licensing pages list
Qt Widgets as available under LGPL v3 or GPL v2, and Qt Core/Gui also provide
open-source licensing choices. SkyAPO's build/package documentation chooses
the LGPL-3.0 route for the shared system Qt libraries; the source/app remains
GPL-2.0-or-later. CMake locates a system LGPL-3.0 license text and installs it
as `licenses/Qt-LGPL-3.0-License.txt`; staged-install and Arch package smoke
tests require it. The Help → About dialog identifies Qt, its runtime version,
dynamic linking and the license-notice location.

This repository does not redistribute or modify Qt. The package depends on
the distribution's `qt6-base`, whose own package is responsible for its Qt
binary build, notices and corresponding library source. The package build
scripts and SkyAPO source are available for relinking against a compatible
system Qt version. Confirm the exact selected Qt license/build and all LGPL
conditions for each distribution/release; the notice alone is not a complete
legal-compliance determination.

## Evidence and remaining work

- CMake links `Qt6::Widgets`, uses shared libraries in the local executable,
  and does not bundle Qt binaries.
- Installed docs include Equalizer APO's GPL text and, when the UI is built,
  the Qt LGPL-3.0 text. The app About dialog identifies both project and Qt
  licensing.
- Resource closure is checked by CTest in both FFTW-enabled and fallback
  configurations when available.
- Still open: review target-distribution Qt packaging/relink instructions and
  perform a release-specific legal review. Do not mark the 1.0 licensing gate
  complete until these are resolved.

References:

- [Qt 6 licensing overview](https://doc.qt.io/qt-6/licensing.html)
- [Qt Widgets licensing](https://doc.qt.io/qt-6/qtwidgets-index.html)
- [Qt LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations)
