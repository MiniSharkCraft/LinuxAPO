# Plugin support status

SkyAPO has native LV2 and CLAP audio-effect hosts. `skyapo plugin list` and `skyapo plugin scan` list both formats; scanning and initialization are performed on the control/configuration thread, not in the audio callback.

LV2 is discovered through Lilv's normal LV2 paths. Config syntax is:

```text
Plugin: LV2 https://example.org/plugins/my-stereo-effect
```

Input control ports may be overridden by symbol in the same directive:

```text
Plugin: LV2 https://example.org/plugins/my-stereo-effect gain=0.75 mix=0.4
```

Use `skyapo plugin info <URI>` to inspect input-port symbols, defaults and declared ranges before configuring overrides. SkyAPO rejects unknown symbols, duplicate assignments, non-finite values and out-of-range values during graph construction, so a failed hot reload leaves the prior graph active. Overrides are initialization values; live automation, UI/control-socket changes and saved plugin state are not implemented yet.

The current host supports audio-only effects with the same number of audio input/output ports as the current selected channel set, plus input control ports initialized to LV2 defaults or config overrides. It rejects missing plugins, unsupported required LV2 host features, event/CV/other non-audio ports, mismatched channel layouts, and latency-reporting plugins (SkyAPO does not yet implement plugin delay compensation). It does not yet expose live parameter changes, bypass, serialization/state, plugin UIs, or process isolation. `tests/plugins/skyapo-test.lv2` is an ABI-compatible test effect; CTest verifies default and overridden gain numerically and executes 1000 variable blocks under the callback allocator audit. A real PipeWire capture/virtual-source test with this plugin plus `Preamp: -6 dB` measured correlation 1 and ratio 0.250594 (−12.0206 dB) over 192000 frames; daemon status showed zero callback allocations/deallocations and zero overruns. This proves the fixture plugin's chain path, not third-party crash or allocator behavior.

CLAP uses the official header/API source pinned as the `upstream/clap` Git submodule (MIT; see its `LICENSE`). Set `CLAP_PATH` to a colon-separated plugin search path when needed; SkyAPO also checks `~/.clap`, `/usr/lib/clap`, `/usr/local/lib/clap`, and `/usr/lib64/clap`, recursively for `.clap` modules. The config directive is:

```text
Plugin: CLAP org.vendor.plugin-id
```

For a parameter override, use a stable numeric CLAP parameter ID (or a unique parameter name):

```text
Plugin: CLAP org.vendor.plugin-id 42=0.75
```

The first CLAP host version supports a single main float32 audio input and output bus with identical channel counts matching the active SkyAPO graph. `skyapo plugin info <CLAP-ID>` reports parameter IDs, names, defaults and ranges. A parameter may be set at graph-build time by numeric ID (preferred) or unique name, e.g. `Plugin: CLAP org.vendor.plugin-id 42=0.75`. Values are validated against CLAP's declared range; unknown, ambiguous, read-only, duplicate or out-of-range assignments reject the graph and preserve the previous live graph. The host sends fixed overrides as preallocated parameter events in each process block; live control/UI changes and plugin output-event handling are not implemented. It implements CLAP's thread-check host extension and serializes plugin processing/lifecycle calls. Plugin state, bypass, latency reporting/compensation, GUI and crash isolation are not implemented. A CLAP plugin returning a processing error produces silence for that block. The test fixture `tests/plugins/test_gain_clap.c` verifies main/audio-thread reporting, metadata and parameter override behavior; it is discovered with `CLAP_PATH`, checked numerically in offline rendering, and runs 1000 varying blocks under the allocator audit. A production CLAP plugin has not yet been validated.

Linux VST2/VST3 and yabridge wrapper testing remain unimplemented. SkyAPO does not embed Wine or modify yabridge. Do not redistribute proprietary Steinberg SDK files without permission. CLAP bundles execute in-process and are not sandboxed; a crashing or malicious plugin can crash/compromise skyapod. CLAP plugin scanning executes each module's CLAP entry initializer in-process as standard CLAP discovery requires. The interfaces in `src/plugin/IPluginInstance.h` are the format-neutral boundary for these hosts.

The Lilv catalog is initialized lazily on first LV2 scan/use and retained once for process lifetime, so ordinary EAPO-only configs do not scan plugin bundles and hot reloads do not rescan them. On the current Arch Lilv 0.28.0 build, LeakSanitizer finds a 24-byte allocation left unreachable by `lilv_world_load_plugin_classes`; sanitizer CTest narrowly suppresses that external stack only. The plugin-list command separately frees all returned plugin-name nodes.
