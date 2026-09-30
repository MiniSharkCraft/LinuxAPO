# VST2 host feasibility prototype

This is an unintegrated experiment, not production VST2 support. It adds an
`IPluginInstance` implementation in `src/plugin/VST2PluginHost.cpp`; it is not
registered with `Engine`, the configuration parser, CMake, package metadata, or
the UI. Its sole API source is the pinned FST submodule at
`upstream/fst` (`647af068765b75867e3a28b4dd8991ab9ed47f7c`). It does not include
Steinberg or Equalizer APO `aeffect.h` / `aeffectx.h` headers.

FST describes itself as an independently reverse-engineered interface and
licenses its header GPL-3.0-or-later. Its README says the interface is not yet
feature-complete and marks unknown opcodes as deprecated. Keep the upstream
license, copyright notices, and reverse-engineering notes with the submodule.
The EAPO source files used by SkyAPO declare GPL version 2 or (at the user's
option) any later version; combining code and distributing a build still needs
a deliberate license review, including applicable GPL obligations. This note
is evidence about the files' notices, not a legal opinion.

FST's README also discusses Steinberg's `VST` trademark and names restrictions
for plugins. It does not resolve how SkyAPO should market or name a host. Obtain
appropriate trademark/licensing review before public distribution; the
prototype's existence is not a legal clearance. Steinberg's current SDK FAQ
addresses its own SDK files and license terms separately; this prototype does
not use those files.

The current code only exercises a host-compatible 32-bit float mono/stereo
effect module path: dynamic load, `VSTPluginMain`/`main` resolution, basic host
callbacks, `AEffect` validation, open/configure/activate/close, parameter-name
and normalized-value lookup, and `processReplacing`. It does not provide
discovery, GUI, state/chunks, automation, MIDI/events, shell plugins, multiple
buses, sample-accurate parameter events, process isolation, or comprehensive
host callback behavior. In-process plugins can crash or compromise the daemon;
C++ exception handling cannot isolate memory faults or hostile code. No claim
is made about compatibility with arbitrary third-party plugins.

## Standalone fixture test

This deliberately bypasses the project build system so this prototype does not
silently become a user-facing feature:

```sh
mkdir -p /tmp/skyapo-vst2-prototype
cc -std=c11 -fPIC -shared -Iupstream/fst/fst \
  tests/plugins/vst2_gain_fixture.c \
  -o /tmp/skyapo-vst2-prototype/vst2_gain_fixture.so
c++ -std=c++17 -Isrc/plugin -Iupstream/fst/fst \
  src/plugin/VST2PluginHost.cpp tests/vst2_host_prototype_test.cpp -ldl \
  -o /tmp/skyapo-vst2-prototype/vst2-host-test
/tmp/skyapo-vst2-prototype/vst2-host-test \
  /tmp/skyapo-vst2-prototype/vst2_gain_fixture.so
```

The fixture is SkyAPO-authored test code built against the FST API; it is not a
third-party compatibility test and should not be installed or advertised as a
user plugin.
