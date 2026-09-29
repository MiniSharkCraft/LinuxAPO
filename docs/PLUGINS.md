# Plugin support status

SkyAPO now has a first native LV2 host through Lilv. It discovers bundles from Lilv's normal LV2 search paths and exposes `skyapo plugin list` / `skyapo plugin scan`. Config syntax is:

```text
Plugin: LV2 https://example.org/plugins/my-stereo-effect
```

Input control ports may be overridden by symbol in the same directive:

```text
Plugin: LV2 https://example.org/plugins/my-stereo-effect gain=0.75 mix=0.4
```

Use `skyapo plugin info <URI>` to inspect input-port symbols, defaults and declared ranges before configuring overrides. SkyAPO rejects unknown symbols, duplicate assignments, non-finite values and out-of-range values during graph construction, so a failed hot reload leaves the prior graph active. Overrides are initialization values; live automation, UI/control-socket changes and saved plugin state are not implemented yet.

The current host supports audio-only effects with the same number of audio input/output ports as the current selected channel set, plus input control ports initialized to LV2 defaults or config overrides. It rejects missing plugins, unsupported required LV2 host features, event/CV/other non-audio ports, mismatched channel layouts, and latency-reporting plugins (SkyAPO does not yet implement plugin delay compensation). It does not yet expose live parameter changes, bypass, serialization/state, plugin UIs, or process isolation. `tests/plugins/skyapo-test.lv2` is an ABI-compatible test effect; CTest verifies default and overridden gain numerically and executes 1000 variable blocks under the callback allocator audit. A real PipeWire capture/virtual-source test with this plugin plus `Preamp: -6 dB` measured correlation 1 and ratio 0.250594 (−12.0206 dB) over 192000 frames; daemon status showed zero callback allocations/deallocations and zero overruns. This proves the fixture plugin's chain path, not third-party crash or allocator behavior.

Native CLAP, Linux VST2/VST3 and yabridge wrapper loading remain unimplemented. SkyAPO does not embed Wine or modify yabridge. Do not redistribute proprietary Steinberg SDK files without permission. Plugin discovery, metadata parsing and instantiation run during configuration graph construction, outside the audio callback; only the plugin's `run()` is called in processing. The interfaces in `src/plugin/IPluginInstance.h` are the format-neutral boundary for future hosts.

The Lilv catalog is initialized lazily on first LV2 scan/use and retained once for process lifetime, so ordinary EAPO-only configs do not scan plugin bundles and hot reloads do not rescan them. On the current Arch Lilv 0.28.0 build, LeakSanitizer finds a 24-byte allocation left unreachable by `lilv_world_load_plugin_classes`; sanitizer CTest narrowly suppresses that external stack only. The plugin-list command separately frees all returned plugin-name nodes.
