# Third-party notices

SkyAPO is a Linux port/evolution of Equalizer APO and builds selected original
Equalizer APO DSP and editor components. Those derived components retain their
upstream copyright headers and are distributed under GNU GPL version 2 or, at
the recipient's option, any later version. The complete upstream license is
`upstream/equalizerapo/License.txt`; binary packages install it as
`licenses/EqualizerAPO-License.txt`.

The product build also uses the following pinned interface/SDK sources. Their
original license texts are installed unchanged in the package's `licenses/`
directory:

- CLAP API headers: MIT License, Copyright (c) 2021 Alexandre BIQUE; source:
  `upstream/clap/LICENSE`.
- Steinberg VST3 `base`, `pluginterfaces`, and `public.sdk` subsets: MIT
  License, Copyright (c) Steinberg Media Technologies GmbH; each pinned
  submodule's `LICENSE.txt` is retained and installed separately.
- MuParserX 3.0.1 parser source: BSD-2-Clause, Copyright (c) Ingo Berg and
  contributors; fetched from the attachment linked by the official Equalizer
  APO developer wiki using a pinned SHA-256. The original `License.txt` is
  installed as `licenses/MuParserX-License.txt`. This version is selected for
  its semicolon expression sequencing used by Equalizer APO.

The optional FST-based VST2-compatible host is disabled by default and in the
Arch package. When `SKYAPO_ENABLE_FST_VST2_HOST=ON`, the build links the pinned
FST interface at `upstream/fst`; its GPL-3.0-or-later license is installed as
`licenses/FST-GPL-3.0-or-later-License.txt`. This experimental integration is
not production VST2 support and requires distribution/legal review; see
`docs/VST2_PROTOTYPE.md`. SkyAPO does not bundle or modify yabridge or Wine.

The Linux editor compiles selected upstream Qt Widgets code and dynamically
links the host distribution's Qt 6 Core/Gui/Widgets libraries. Qt Widgets is
available under LGPL-3.0 or GPL-2.0; SkyAPO's package path uses the LGPL-3.0
option. The installed documentation includes `Qt-LGPL-3.0-License.txt`, and
the editor's Help → About dialog identifies Qt use and the license. The Arch
package depends on the system `qt6-base` package and does not redistribute Qt
binaries. The executable uses shared Qt libraries from the user's system,
allowing replacement by a compatible system Qt build. Refer to the host
distribution's Qt package for its exact build, notices, and corresponding
library sources.

The full upstream `Editor.qrc` is intentionally not embedded. SkyAPO provides
the resource paths expected by selected upstream row/GraphicEQ widgets using
project-authored SVG artwork under `src/ui/resources/icons/`. Upstream
translations, Qt-base translations, and the pink-noise calibration FLAC (used
by an excluded Windows-dependent editor) are omitted. The pinned upstream tree
does not provide per-icon license metadata; its icon artwork is not included in
these resources. The Qt/distribution license configuration still needs
release-specific review.

Other libraries (including PipeWire, libsndfile, FFTW, Lilv and muParser) are
discovered from and linked to the host distribution; this project does not
redistribute their binaries. Their corresponding source packages and license
texts remain the distribution's responsibility. The Arch package metadata
declares the license identifiers confirmed for the application and pinned
dependencies. Staged-install tests check the installed license texts and Qt
notice.

This notice is an inventory of the sources used by the current build, not a
substitute for the full license texts or a legal opinion. See the source
submodules and their preserved notices for complete terms.
