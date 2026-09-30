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

The optional FST-based VST2 compatibility experiment is test-only, opt-in, and
is not linked into or installed with the product package. It is not a shipped
VST2 implementation. SkyAPO does not bundle or modify yabridge or Wine.

Other libraries (including PipeWire, libsndfile, FFTW, Lilv, muParser and Qt)
are discovered from and linked to the host distribution; this project does not
redistribute their binaries. Their corresponding source packages and license
texts remain the distribution's responsibility. The Arch package metadata
declares the applicable project license identifiers, and package tests check
that the GPL, MIT and BSD license texts are present after installation.

This notice is an inventory of the sources used by the current build, not a
substitute for the full license texts or a legal opinion. See the source
submodules and their preserved notices for complete terms.
