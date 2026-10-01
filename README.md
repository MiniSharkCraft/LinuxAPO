# LinuxAPO (SkyAPO)

[![Linux](https://img.shields.io/badge/Linux-supported-FCC624?logo=linux&logoColor=black)](https://www.kernel.org/)
[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](https://isocpp.org/)
[![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?logo=cmake&logoColor=white)](https://cmake.org/cmake/help/latest/)
[![PipeWire](https://img.shields.io/badge/PipeWire-realtime-a83b8f)](https://pipewire.org/)
[![Release](https://img.shields.io/badge/release-v0.9.0_beta-blue)](https://github.com/MiniSharkCraft/LinuxAPO/tree/v0.9.0)
[![License](https://img.shields.io/badge/license-GPL--2.0--or--later-green)](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/COPYING)
[![Qt](https://img.shields.io/badge/Qt-6%20optional-41CD52?logo=qt&logoColor=white)](https://doc.qt.io/qt-6/)
[![Nix](https://img.shields.io/badge/Nix-flake-5277C3?logo=nixos&logoColor=white)](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/flake.nix)

**LinuxAPO (project name: SkyAPO)** is a native Linux port of Equalizer APO using the upstream DSP engine, PipeWire realtime audio processing, and native Linux plugin hosting.

**Version 0.9.0 beta — daily-use preview.** This is not 1.0.0 and is not production-final. Hardware coverage, legacy config compatibility, third-party plugin behavior and package lifecycle validation remain limited.

## Features

- Native PipeWire capture → actual Equalizer APO DSP filters → **SkyAPO Virtual Mic**.
- Stable PipeWire input-device selection, daemon status and diagnostics.
- Transactional config validation/reload; a rejected config does not replace the active graph.
- Offline WAV renderer using the same DSP core.
- Optional Qt 6 configuration editor and user-scoped daemon service.
- Native LV2, CLAP and VST3 hosting within the format-specific limits described below.

## Audio path

```text
Physical microphone / capture device
                 │
                 ▼
          PipeWire capture
                 │
                 ▼
     Equalizer APO DSP filters
                 │
                 ▼
       PipeWire virtual source
                 │
                 ▼
      SkyAPO Virtual Mic ──► Discord / browser / OBS / recorder
```

SkyAPO uses native PipeWire APIs. It does not use `pactl move-*`, `ffmpeg`, `ffplay` or external audio processes as its realtime processing architecture.

## Equalizer APO and config compatibility

LinuxAPO does **not** reimplement EQ math. It keeps the official Equalizer APO source as a Git submodule and compiles actual upstream DSP/filter implementations, including Preamp, BiQuad, IIR, Delay, channel/copy routing, GraphicEQ, Convolution and LoudnessCorrection where supported by the build/runtime. Linux adapters replace the Windows APO/COM integration and connect the processing engine to PipeWire. This is not a complete port of the Windows `FilterEngine`, parser or every directive.

The currently supported config subset includes:

- `Preamp:`, parametric and IIR `Filter:`
- `Delay:`, `Channel:`, `Copy:`, `Include:`, `Device:`, `Stage:`
- `If:`, `ElseIf:`, `Else:`, `EndIf:` for the documented expression subset
- `GraphicEQ:` and `Convolution:` when built with FFTW3f
- `LoudnessCorrection:` in daemon mode when the PipeWire volume provider is available

Unsupported active directives are reported with file/line diagnostics rather than silently ignored. See [config compatibility](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/CONFIG.md) and the [Windows config migration guide](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/MIGRATION.md).

Example from `examples/basic.txt`:

```text
Preamp: -6 dB
Filter: ON PK Fc 100 Hz Gain 6 dB Q 1.0
```

## Plugin hosting

- **LV2:** native host when Lilv is available.
- **CLAP:** native host.
- **VST3:** native host for single-main-bus mono/stereo effects.

Hosts provide a limited set of parameter/config override, host bypass, state and reported-latency/PDC features depending on format. See [plugin support and limitations](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/PLUGINS.md). Fixture tests and PipeWire E2E tests verify specific paths; they do not establish compatibility with arbitrary plugins.

Plugins currently run **in-process**. A plugin crash or hang may crash or stall the daemon. Use trusted plugins only. Auxiliary buses, plugin UI, full end-to-end latency reporting and process isolation are not supported.

An FST-based VST2-ABI path is **experimental and opt-in**; it is disabled in default builds and packages. LinuxAPO does not bundle yabridge, Wine, Windows plugins or user plugin binaries. Some yabridge-generated Linux wrappers may load as ordinary Linux plugins, but support and realtime behavior are not guaranteed; observed wrappers may report `realtime: no`. See [VST2 prototype notes](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/VST2_PROTOTYPE.md).

## Editor, daemon and CLI

The optional Qt 6 Widgets editor provides device/daemon/config workflows, preserves untouched config text and offers visual editing for selected filters. It does not provide a plugin GUI, syntax highlighting or full Windows editor parity. See [UI notes](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/UI.md).

The daemon runs as the current user; it does not require root. Common commands:

```sh
skyapo device list
skyapo device set <node-name>
skyapo device current
skyapo start
skyapo status
skyapo diagnostics
skyapo config check ~/.config/skyapo/config.txt
skyapo config reload
skyapo stop
```

## Installation

**GitHub Release assets have not been uploaded yet.** No download links are provided until the assets exist. The following candidate artifacts were built and checked locally; they are not currently downloadable from this repository:

- Arch Linux: `skyapo-0.9.0-9-x86_64.pkg.tar.zst`
- Debian/Ubuntu amd64: `skyapo_0.9.0_amd64.deb`
- RPM x86_64: `skyapo-0.9.0-1.x86_64.rpm`
- Generic Linux x86_64: `skyapo-0.9.0-linux-x86_64.tar.zst`

Until assets are published, build the Arch package from a complete source checkout with `cd packaging && makepkg -si`, or build other packages from source as described in [build and packaging notes](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/BUILDING.md). The generic tar archive targets a dynamic host ABI and is not a cross-distribution runtime guarantee.

A Nix flake package is available for `x86_64-linux`. The package intentionally omits the optional Qt editor and hardware-dependent E2E tests:

```sh
nix build --extra-experimental-features 'nix-command flakes' .#skyapo
nix run --extra-experimental-features 'nix-command flakes' .#skyapo -- --version
nix flake check --extra-experimental-features 'nix-command flakes'
```

## Build from source

Requirements include CMake 3.20+, a C++17 compiler, pkg-config, PipeWire development files and libsndfile. FFTW3f, Lilv and Qt 6 Widgets are optional. The source tree contains a SHA-256-pinned MuParserX archive for reproducible builds.

```sh
git clone --recurse-submodules https://github.com/MiniSharkCraft/LinuxAPO.git
cd LinuxAPO
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

For distro dependencies, sanitizers and packaging details, see [docs/BUILDING.md](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/BUILDING.md).

## Quick start

After installation or building:

```sh
skyapo device list
skyapo device set <node-name>
skyapod --config examples/basic.txt
```

In another terminal, check the live daemon and select **SkyAPO Virtual Mic** in your recording application:

```sh
skyapo status
skyapo diagnostics
```

Alternatively, use the installed user service:

```sh
systemctl --user enable --now skyapod
```

The default config is `$XDG_CONFIG_HOME/skyapo/config.txt`, falling back to `~/.config/skyapo/config.txt`. Device selection persists the stable PipeWire node name rather than only a runtime numeric ID. Validate a config before enabling it:

```sh
skyapo config check examples/basic.txt
```

## Verification

The following results were recorded for the 0.9.0 candidate on the current Linux workstation (2026-10-01):

- PipeWire-enabled CTest: **58/58 passed**, including private-graph capture, source/server recovery, config reload and plugin/PDC E2E tests.
- Opt-in FST/VST2 fixture suite: **57/57 passed**; this does not imply general VST2 product support.
- ASan/UBSan/LeakSanitizer suite: **35/35 passed**; this sanitizer run did not include PipeWire hardware E2E.
- Nix flake package build/check and package smoke: **passed**.
- Arch, DEB, RPM and generic tar candidate checksums and staged payload smoke: **passed**. These checks are not equivalent to dependency-populated clean installs on every target distribution.
- Physical PipeWire recording and numerical `Preamp: -6 dB` measurement are documented in [realtime verification](docs/REALTIME.md).

These are local recorded results, not hosted CI results or certification across machines.

## Current limitations and deferred to 1.0.0

- Full Windows Equalizer APO parser, directive, editor and device semantics are not implemented.
- Physical microphone unplug/replug and automatic desktop-client source following are not verified across hardware and applications.
- Dependency-populated clean install/uninstall and Arch ALPM hook execution have not been fully verified in a clean root; extracted-payload smoke is not equivalent.
- Hosted CI results are pending.
- Legal/distribution review and VST2/FST scope remain open; the experimental host is disabled by default.
- Plugin crash/hang isolation, auxiliary buses, broad third-party/yabridge compatibility and total end-to-end latency remain unverified or unsupported.
- Hardware recording evidence currently covers one Linux/PipeWire workstation. Package assets still need to be uploaded separately.

See the [1.0.0 deferred backlog](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/ROADMAP.md#deferred-to-100) for details. **0.9.0 is a beta daily-use preview, not production-final.**

## Contributing

Bug reports should include the distribution, PipeWire version, daemon diagnostics, a minimal sanitized config and reproduction steps. Do not attach microphone recordings, plugin binaries or credentials. Start with [architecture](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/ARCHITECTURE.md), [porting notes](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/PORTING.md) and [troubleshooting](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/TROUBLESHOOTING.md).

Runtime/audio changes should be tested proportionally: CTest, sanitizers where appropriate and private PipeWire E2E. A “daemon running” message alone does not prove processed samples reached the virtual microphone; verify recorded output independently.

## Attribution and license

LinuxAPO reuses and adapts DSP/editor code from **Equalizer APO** by Jonas Thedering. The upstream source, Git history and copyright headers are preserved separately at `upstream/equalizerapo`. See the [official SourceForge repository](https://git.code.sf.net/p/equalizerapo/code), the [porting audit](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/PORTING.md) and [third-party notices](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/THIRD_PARTY_NOTICES.md).

Equalizer APO's license states GNU General Public License version 2, **or (at your option) any later version**. The complete text is in [COPYING](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/COPYING) and the upstream submodule at `upstream/equalizerapo/License.txt`. SPDX identifier: `GPL-2.0-or-later`. Third-party SDKs and dependencies retain their own licenses; consult their notices and pinned submodules. LinuxAPO does not bundle user plugins, yabridge, Wine or Windows plugin binaries.

## Acknowledgements

Thanks to Jonas Thedering and Equalizer APO contributors, and to the CLAP, Steinberg VST3, FST, MuParserX, PipeWire, Lilv, FFTW, libsndfile, Qt and other upstream projects whose DSP, APIs and tools this project builds upon. Their specific license texts and attributions are listed in [third-party notices](https://github.com/MiniSharkCraft/LinuxAPO/blob/main/docs/THIRD_PARTY_NOTICES.md).
